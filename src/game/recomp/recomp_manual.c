/**
 * Burnout 3 - Manually implemented recompiled functions
 *
 * These are functions that the automatic recompiler couldn't handle
 * correctly (mid-function entry points, SEH continuations, etc.).
 * They use the same register model and calling conventions as the
 * generated code.
 */

#define RECOMP_GENERATED_CODE
#include "gen/recomp_funcs.h"
#include "../static_textures.h"
#include "../track_loader.h"
#include <math.h>
#include <stdio.h>

/* xbox_HeapAlloc is defined further down in this file but is first
 * referenced inside another function -- forward declare it. */
extern uint32_t xbox_HeapAlloc(uint32_t size, uint32_t alignment);

/* ICALL failure diagnostic (rate-limited).
 *
 * A failed indirect call silently pops its dummy return address, sets eax = 0
 * and carries on. That is the right thing for a game that pokes garbage
 * function pointers -- but it also means a function stuck calling a NULL
 * pointer spins for ever in total silence. Which is exactly what happens after
 * the boot: 32M failed ICALLs a second, all to VA 0.
 *
 * _ReturnAddress() gives the address inside the GENERATED function that made
 * the call, which bin/burnout3.map turns straight back into a name. */
void recomp_icall_fail_log(uint32_t va)
{
    static volatile LONG s_count = 0;
    LONG n = InterlockedIncrement(&s_count);

    /* Loud for the first few, then once a million so a spin is visible without
     * drowning the log. */
    if (n <= 12 || (n % 1000000) == 0) {
        /* We run on the calling thread, so the register set here is the
         * caller's. edi/esi are the object and index in sub_001CA530's loop. */
        fprintf(stderr, "  [ICALL-FAIL #%ld] target va=0x%08X, from native %p "
                "(edi=0x%08X esi=0x%08X ecx=0x%08X)\n",
                (long)n, va, _ReturnAddress(), g_edi, g_esi, g_ecx);

        /* Native backtrace: recompiled functions are real C functions calling
         * each other, so the host stack IS the Xbox call chain. This is how we
         * find who passed the bad object, rather than guessing between the
         * five callers the xref data lists. */
        {
            void *frames[12];
            USHORT got = CaptureStackBackTrace(0, 12, frames, NULL);
            fprintf(stderr, "      stack:");
            for (USHORT i = 0; i < got; i++)
                fprintf(stderr, " %p", frames[i]);
            fprintf(stderr, "\n");
        }
        fflush(stderr);
    }
}

/* D3D8 frame pump (implemented in d3d8_device.c) */
extern void d3d8_PresentFrame(void);

/* D3D8 device access */
#include "d3d8_xbox.h"
#include "d3d8_internal.h"

/* TXD texture loader */
#include "txd_loader.h"

/* RW→D3D11 rendering bridge (rw_bridge.c) */
extern int  rw_bridge_camera_render(uint32_t camera_va);
extern void rw_bridge_camera_begin(uint32_t camera_va);
extern void rw_bridge_camera_end(uint32_t camera_va);
extern void rw_bridge_new_frame(void);
extern int  rw_bridge_frame_rendered(void);
extern int  rw_bridge_im2d_render(int prim_type, const void *verts, int vert_count);

/**
 * RW Display Driver Function Pointer Table
 *
 * The RW engine stores display driver callbacks in a table at 0x7592E8.
 * sub_001DE190 normally populates this during RW device initialization,
 * but it's called via ICALL which may fail. We populate it manually.
 *
 * Table layout: MEM32(0x7592E8 + id*4) = function_address
 * Known entries (from xemu analysis session 33):
 *   ID 0x01 -> 0x001DD910  (display open/start)
 *   ID 0x0A -> 0x001DDAE0  (display reset)
 *   ID 0x14 -> 0x001DDAF0  (render callback) [0x759338]
 *   ID 0x0C -> 0x001DCF50  (viewport setup)  [0x759318]
 *   ...and ~25 more entries
 */
void rw_init_display_driver_table(void)
{
    /* Populate the function pointer table that sub_001DE190 would fill.
     * Base address: 0x7592E8 (confirmed: 0x7592E8 + 0x14*4 = 0x759338) */
    static const struct { uint32_t id; uint32_t func; } entries[] = {
        { 0x01, 0x1DD910 },   /* display start */
        { 0x0A, 0x1DDAE0 },   /* display reset */
        { 0x15, 0x1DE0F0 },   /* device close */
        { 0x14, 0x1DDAF0 },   /* render callback (the key one!) */
        { 0x02, 0x1E48E0 },
        { 0x03, 0x1E4BE0 },
        { 0x07, 0x1E5870 },
        { 0x06, 0x1E52C0 },
        { 0x05, 0x1DCDC0 },
        { 0x04, 0x1DCB70 },
        { 0x09, 0x1E5AE0 },
        { 0x08, 0x1DCF40 },
        { 0x0F, 0x1DBDE0 },
        { 0x10, 0x1DC150 },
        { 0x17, 0x1DC2D0 },
        { 0x18, 0x1DC360 },
        { 0x0E, 0x1E7AD0 },
        { 0x0D, 0x1E7AB0 },
        { 0x11, 0x1E7730 },
        { 0x12, 0x1E7770 },
        { 0x13, 0x1E7750 },
        { 0x0B, 0x1E7B10 },
        { 0x0C, 0x1DCF50 },   /* viewport setup */
        { 0x19, 0x1E6710 },
        { 0x1B, 0x1E67B0 },
        { 0x1A, 0x1E6A60 },
        { 0x1C, 0x1DCEB0 },
    };
    uint32_t table_base = 0x7592E8;
    int i;

    /* First fill with default handler (0x24B90 = RW default stub) */
    for (i = 0; i < 0x20; i++) {
        MEM32(table_base + i * 4) = 0x24B90;
    }

    /* Then populate known entries */
    for (i = 0; i < (int)(sizeof(entries)/sizeof(entries[0])); i++) {
        MEM32(table_base + entries[i].id * 4) = entries[i].func;
    }

    /* Also populate the im2d function pointer table.
     * sub_001DBC80 normally does this during RW device open, but
     * RW device initialization doesn't fully execute in our recomp.
     * These are the im2d vertex transform and render functions. */
    MEM32(0x7592A8) = 0x1DB620;   /* im2d vertex transform (single) */
    MEM32(0x7592AC) = 0x1DB2C0;   /* im2d transform helper */
    MEM32(0x7592B0) = 0x1DB9D0;   /* im2d vertex transform (inverse) */
    MEM32(0x7592B4) = 0x1DB6D0;   /* im2d batch vertex transform */
    MEM32(0x41AAD4) = 1;          /* im2d reference count */

    /* Im2d render callbacks (stored in im2d state block):
     * These are set by RW device open (sub_001E3931 path) but we
     * need to ensure they're populated for menu/HUD rendering. */
    MEM32(0x7592CC) = 0x1E2930;   /* im2d render triangle callback */
    MEM32(0x7592D0) = 0x1E2330;   /* im2d render line callback */

    fprintf(stderr, "  [RW] Display driver table populated at 0x%08X\n", table_base);
    fprintf(stderr, "  [RW]   [0x759338] (render cb) = 0x%08X\n", MEM32(0x759338));
    fprintf(stderr, "  [RW]   [0x759318] (viewport)  = 0x%08X\n", MEM32(0x759318));
    fprintf(stderr, "  [RW]   [0x7592B4] (im2d fn)   = 0x%08X\n", MEM32(0x7592B4));
}

/* Track spawn state exported from rw_renderer.c */
extern float g_track_spawn_x;
extern float g_track_spawn_z;
extern float g_track_spawn_hdg;
extern int   g_track_mode;

/* RW linked-list traversal with loop limit */
void sub_001FE1E0(void);

void sub_00351090(void);
void sub_00351490(void);
void sub_00350C10(void);

/* Forward declarations for manually implemented functions */
void sub_0002DDF0(void);
void sub_001BEFF0(void);
void sub_00011000(void);
void sub_001F5810(void);
void sub_001F5840(void);
void sub_001F5C40(void);
void sub_001F5CB0(void);
void sub_001CFDD0(void);
void sub_001D1818(void);
void sub_001D2793(void);
void sub_001D5707(void);
void sub_001D5E82(void);
void sub_00244C51(void);
void sub_00249B7C(void);
void sub_00249B9C(void);
void sub_003518E0(void);
void sub_00351770(void);
void sub_00351A20(void);
void sub_0034CBF0(void);
void sub_0034CEF0(void);
void sub_0034F5B0(void);
void sub_003558A0(void);
void sub_0034D410(void);
void sub_0003FEE0(void);
void sub_001C1670(void);
void sub_001D7180(void);
void sub_001D7857(void);
void sub_001D7876(void);
void sub_001D7D90(void);
void sub_001D88E0(void);
void sub_001D8A80(void);
void sub_001D8FA0(void);
void sub_001D9180(void);
void sub_001D91B0(void);
void sub_001D91F0(void);
void sub_001D9230(void);
/* sub_00157680 removed from manual overrides - uses gen code with relocated PrgData */
void sub_001D9280(void);
void sub_001DDAF0(void);
void sub_001DD910(void);  /* CameraBeginUpdate - display driver 0x01 */
void sub_001E7B10(void);  /* CameraEndUpdate - display driver 0x0B */
void sub_001D9290(void);
void sub_001D92A0(void);
void sub_001D92EF(void);
void sub_001D9360(void);
void sub_001D93AF(void);
void sub_000171A0(void);  /* Frontend render dispatch (traced) */
void sub_001D9420(void);
void sub_001D9450(void);
void sub_001D94A0(void);
void sub_001D94D0(void);
void sub_001D9510(void);
void sub_001D7D10(void);
void sub_001D7D50(void);
void sub_001D7D70(void);
void sub_001D9700(void);
void sub_001D9A50(void);
void sub_001D9AF0(void);
void sub_001D9BC0(void);
void sub_001D9D40(void);
void sub_001C1740(void);
void sub_0034C2E0(void);  /* D3D state machine (83K, native ptr crash) */
void sub_001AE6F0(void);  /* Frontend render dispatch (override) */
void sub_001AE732(void);  /* Mid-function entry of sub_001AE6F0 (stub) */

/* Newly-exposed mid-function entry point stubs (switch table fix) */
void sub_00014FB0(void);
void sub_0006AE80(void);
void sub_000983E0(void);
void sub_000E32F0(void);
void sub_00154270(void);
void sub_0015BC50(void);
void sub_00169BD0(void);
void sub_00188BA9(void);
void sub_0018BF50(void);
void sub_001BDD50(void);
void sub_002F55AC(void);
void sub_002F6770(void);
void sub_0031AAE7(void);
void sub_0031AB7A(void);
void sub_0031AB90(void);
void sub_0031ABB1(void);
void sub_0031ABDD(void);
void sub_0031ABE8(void);
void sub_0034FBA0(void);
/* Round 2: more mid-function entry points */
void sub_0006E680(void);
void sub_0008E8D0(void);
void sub_00090A27(void);
void sub_0009E127(void);
void sub_000A0BF0(void);
void sub_000A7410(void);
void sub_000E0080(void);
void sub_00200470(void);
void sub_0031ABD2(void);
void sub_0031AC0D(void);
void sub_003392F8(void);
void sub_003394FB(void);
void sub_00339506(void);
void sub_00339511(void);
void sub_0035B3B0(void);
void sub_00361BB4(void);

/* Im2D vertex transform helper (not generated - below recompiler threshold) */
void sub_001DB2C0(void);

/* D3D8LTCG rendering pipeline stubs */
void sub_0034D530(void);

/* Rendering context tick (stubbed - Xbox rendering pipeline not needed) */
void sub_000110E0(void);

/* Audio/streaming init (stubbed - hangs in RW pipe iteration) */
void sub_00135040(void);

/* Resource slot polling (overridden - skip version check for deferred workers) */
void sub_00018BB0(void);

/* RW resource fixup (stubbed - resources not worker-processed) */
void sub_00020930(void);

/* RW resource pointer relocation (stubbed - resources not worker-processed) */
void sub_00159710(void);

/* Track/scene setup (stubbed - depends on valid RW world data) */
void sub_0001BE60(void);

/* Audio streaming setup (stubbed - audio init was skipped) */
void sub_00135240(void);

/* RW stream reader (override - marks stream complete without vtable walks) */
void sub_001B33A0(void);

/* Track environment loader (stubbed - RW stream reader hangs) */
void sub_00062BD0(void);

/* Pipeline/material name lookup (stubbed - RW world not initialized) */
void sub_0004DD00(void);

/* RW hash table lookup (safe - guards against div-by-zero) */
void sub_00221F20(void);

/* RW world linked list cleanup (stubbed - world data not initialized) */
void sub_001C66F0(void);

/* Resource queue handler (override - actually loads files) */
void sub_00011240(void);

/* Game state notification dispatch (recursion-guarded) */
void sub_00022660(void);

/* Car physics force computation (overridden - scale factor fallbacks) */
void sub_000636D0(void);


/* ── Manual dispatch table ────────────────────────────────────────────
 *
 * Functions defined in this file that aren't in the auto-generated
 * dispatch table (because gen/ is gitignored and regenerated).
 * recomp_lookup_manual() is called by RECOMP_ICALL as a fallback.
 */
static const struct {
    uint32_t xbox_va;
    recomp_func_t func;
} g_manual_funcs[] = {
    { 0x0002DDF0u, (recomp_func_t)sub_0002DDF0 },
    { 0x001BEFF0u, (recomp_func_t)sub_001BEFF0 },
    { 0x001CFDD0u, (recomp_func_t)sub_001CFDD0 },
    { 0x001D1818u, (recomp_func_t)sub_001D1818 },
    { 0x001D2793u, (recomp_func_t)sub_001D2793 },
    { 0x001D5707u, (recomp_func_t)sub_001D5707 },
    { 0x001F5810u, (recomp_func_t)sub_001F5810 },
    { 0x001F5840u, (recomp_func_t)sub_001F5840 },
    { 0x001F5C40u, (recomp_func_t)sub_001F5C40 },
    { 0x001F5CB0u, (recomp_func_t)sub_001F5CB0 },
    { 0x001D5E82u, (recomp_func_t)sub_001D5E82 },
    { 0x00244C51u, (recomp_func_t)sub_00244C51 },
    { 0x00249B7Cu, (recomp_func_t)sub_00249B7C },
    { 0x00249B9Cu, (recomp_func_t)sub_00249B9C },
    { 0x003518E0u, (recomp_func_t)sub_003518E0 },
    { 0x00011000u, (recomp_func_t)sub_00011000 },
    { 0x00351770u, (recomp_func_t)sub_00351770 },
    { 0x00351A20u, (recomp_func_t)sub_00351A20 },
    /* sub_0034CBF0 + sub_0034CEF0 use gen code (PB cursor reset in sub_003518E0) */
    { 0x0034F5B0u, (recomp_func_t)sub_0034F5B0 },
    { 0x003558A0u, (recomp_func_t)sub_003558A0 },
    { 0x0034D410u, (recomp_func_t)sub_0034D410 },
    { 0x0003FEE0u, (recomp_func_t)sub_0003FEE0 },
    { 0x001C1670u, (recomp_func_t)sub_001C1670 },
    { 0x001D7180u, (recomp_func_t)sub_001D7180 },
    { 0x001D7857u, (recomp_func_t)sub_001D7857 },
    { 0x001D7876u, (recomp_func_t)sub_001D7876 },
    { 0x001D7D90u, (recomp_func_t)sub_001D7D90 },
    { 0x001D88E0u, (recomp_func_t)sub_001D88E0 },
    { 0x001D8A80u, (recomp_func_t)sub_001D8A80 },
    { 0x001D8FA0u, (recomp_func_t)sub_001D8FA0 },
    { 0x001D9180u, (recomp_func_t)sub_001D9180 },
    { 0x001D91B0u, (recomp_func_t)sub_001D91B0 },
    { 0x001D91F0u, (recomp_func_t)sub_001D91F0 },
    { 0x001D9230u, (recomp_func_t)sub_001D9230 },
    { 0x001D9280u, (recomp_func_t)sub_001D9280 },
    { 0x001D9290u, (recomp_func_t)sub_001D9290 },
    { 0x001D92A0u, (recomp_func_t)sub_001D92A0 },
    { 0x001D92EFu, (recomp_func_t)sub_001D92EF },
    { 0x001D9360u, (recomp_func_t)sub_001D9360 },
    { 0x001D93AFu, (recomp_func_t)sub_001D93AF },
    { 0x001D9420u, (recomp_func_t)sub_001D9420 },
    { 0x001D9450u, (recomp_func_t)sub_001D9450 },
    { 0x001D94A0u, (recomp_func_t)sub_001D94A0 },
    { 0x001D94D0u, (recomp_func_t)sub_001D94D0 },
    { 0x001D9510u, (recomp_func_t)sub_001D9510 },
    { 0x001D7D10u, (recomp_func_t)sub_001D7D10 },
    { 0x001D7D50u, (recomp_func_t)sub_001D7D50 },
    { 0x001D7D70u, (recomp_func_t)sub_001D7D70 },
    { 0x001D9700u, (recomp_func_t)sub_001D9700 },
    { 0x001DDAF0u, (recomp_func_t)sub_001DDAF0 },
    { 0x001DD910u, (recomp_func_t)sub_001DD910 },  /* CameraBeginUpdate */
    { 0x001E7B10u, (recomp_func_t)sub_001E7B10 },  /* CameraEndUpdate */
    { 0x00351090u, (recomp_func_t)sub_00351090 },
    { 0x001D9A50u, (recomp_func_t)sub_001D9A50 },
    { 0x001D9AF0u, (recomp_func_t)sub_001D9AF0 },
    { 0x001D9BC0u, (recomp_func_t)sub_001D9BC0 },
    { 0x001D9D40u, (recomp_func_t)sub_001D9D40 },
    { 0x001C1740u, (recomp_func_t)sub_001C1740 },
    { 0x0034C2E0u, (recomp_func_t)sub_0034C2E0 },
    { 0x001AE6F0u, (recomp_func_t)sub_001AE6F0 },
    { 0x001AE732u, (recomp_func_t)sub_001AE732 },
    /* Mid-function entry points exposed by switch table fix */
    { 0x00014FB0u, (recomp_func_t)sub_00014FB0 },
    { 0x0006AE80u, (recomp_func_t)sub_0006AE80 },
    { 0x000983E0u, (recomp_func_t)sub_000983E0 },
    { 0x000E32F0u, (recomp_func_t)sub_000E32F0 },
    { 0x00154270u, (recomp_func_t)sub_00154270 },
    { 0x0015BC50u, (recomp_func_t)sub_0015BC50 },
    { 0x00169BD0u, (recomp_func_t)sub_00169BD0 },
    { 0x00188BA9u, (recomp_func_t)sub_00188BA9 },
    { 0x0018BF50u, (recomp_func_t)sub_0018BF50 },
    { 0x001BDD50u, (recomp_func_t)sub_001BDD50 },
    { 0x002F55ACu, (recomp_func_t)sub_002F55AC },
    { 0x002F6770u, (recomp_func_t)sub_002F6770 },
    { 0x0031AAE7u, (recomp_func_t)sub_0031AAE7 },
    { 0x0031AB7Au, (recomp_func_t)sub_0031AB7A },
    { 0x0031AB90u, (recomp_func_t)sub_0031AB90 },
    { 0x0031ABB1u, (recomp_func_t)sub_0031ABB1 },
    { 0x0031ABDDu, (recomp_func_t)sub_0031ABDD },
    { 0x0031ABE8u, (recomp_func_t)sub_0031ABE8 },
    { 0x0034FBA0u, (recomp_func_t)sub_0034FBA0 },
    /* Round 2 mid-function entry points */
    { 0x0006E680u, (recomp_func_t)sub_0006E680 },
    { 0x0008E8D0u, (recomp_func_t)sub_0008E8D0 },
    { 0x00090A27u, (recomp_func_t)sub_00090A27 },
    { 0x0009E127u, (recomp_func_t)sub_0009E127 },
    { 0x000A0BF0u, (recomp_func_t)sub_000A0BF0 },
    { 0x000A7410u, (recomp_func_t)sub_000A7410 },
    { 0x000E0080u, (recomp_func_t)sub_000E0080 },
    { 0x00200470u, (recomp_func_t)sub_00200470 },
    { 0x0031ABD2u, (recomp_func_t)sub_0031ABD2 },
    { 0x0031AC0Du, (recomp_func_t)sub_0031AC0D },
    { 0x003392F8u, (recomp_func_t)sub_003392F8 },
    { 0x003394FBu, (recomp_func_t)sub_003394FB },
    { 0x00339506u, (recomp_func_t)sub_00339506 },
    { 0x00339511u, (recomp_func_t)sub_00339511 },
    { 0x0035B3B0u, (recomp_func_t)sub_0035B3B0 },
    { 0x00361BB4u, (recomp_func_t)sub_00361BB4 },
    /* RW linked-list traversal with loop limit */
    { 0x001FE1E0u, (recomp_func_t)sub_001FE1E0 },
    /* Im2D vertex transform helper (stub) */
    { 0x001DB2C0u, (recomp_func_t)sub_001DB2C0 },
    /* D3D8LTCG rendering pipeline */
    { 0x0034D530u, (recomp_func_t)sub_0034D530 },
    /* Rendering context tick (stubbed) */
    { 0x000110E0u, (recomp_func_t)sub_000110E0 },
    { 0x00020930u, (recomp_func_t)sub_00020930 },
    /* RW resource relocation (stubbed) */
    { 0x00159710u, (recomp_func_t)sub_00159710 },
    /* Track/scene setup (stubbed) */
    { 0x0001BE60u, (recomp_func_t)sub_0001BE60 },
    /* Audio streaming setup (stubbed) */
    { 0x00135240u, (recomp_func_t)sub_00135240 },
    /* Track environment loader (stubbed) */
    { 0x00062BD0u, (recomp_func_t)sub_00062BD0 },
    /* Pipeline/material name lookup (stubbed) */
    { 0x0004DD00u, (recomp_func_t)sub_0004DD00 },
    /* RW hash table lookup (div-by-zero safe) */
    { 0x00221F20u, (recomp_func_t)sub_00221F20 },
    /* RW world linked list cleanup (stubbed) */
    { 0x001C66F0u, (recomp_func_t)sub_001C66F0 },
    /* Game state notification dispatch (recursion-guarded) */
    { 0x00022660u, (recomp_func_t)sub_00022660 },
    /* Audio/streaming init (stubbed) */
    { 0x00135040u, (recomp_func_t)sub_00135040 },
    /* Resource slot polling (version-check bypass) */
    { 0x00018BB0u, (recomp_func_t)sub_00018BB0 },
    /* Car physics force computation (scale factor fallbacks) */
    { 0x000636D0u, (recomp_func_t)sub_000636D0 },
    /* Resource queue handler (override - actually loads files) */
    { 0x00011240u, (recomp_func_t)sub_00011240 },
    /* sub_00157680 removed - using gen code with relocated PrgData */
    /* Frontend render dispatch (traced) */
    { 0x000171A0u, (recomp_func_t)sub_000171A0 },
    /* Game mode state machine (force state 4→5 transition) */
    { 0x001AA100u, (recomp_func_t)sub_001AA100 },
    /* RW stream reader (override - marks complete without vtable walks) */
    { 0x001B33A0u, (recomp_func_t)sub_001B33A0 },
};
#define NUM_MANUAL_FUNCS (sizeof(g_manual_funcs) / sizeof(g_manual_funcs[0]))

recomp_func_t recomp_lookup_manual(uint32_t xbox_va)
{
    for (size_t i = 0; i < NUM_MANUAL_FUNCS; i++) {
        if (g_manual_funcs[i].xbox_va == xbox_va)
            return g_manual_funcs[i].func;
    }
    return NULL;
}

/**
 * sub_001BEFF0 - RW memory pool free list reorganization (STUB)
 *
 * Original: 0x001BEFF0 - 0x001BF03A (74 bytes, 32 insns)
 * Category: game_engine (RenderWare core)
 *
 * This function reorganizes a memory pool's free list by scanning the
 * contiguous block array and relinking free nodes in address order.
 * The pool descriptor is passed in esi (register parameter):
 *   esi+0x00 = head pointer (contiguous array base)
 *   esi+0x04 = block stride
 *   esi+0x10 = free list head
 *
 * When the pool contains uninitialized data (because the Xbox D3D cache
 * wasn't properly initialized), the free list walk reads garbage pointers
 * and enters a loop that exhausts all 50,000 VEH fault-skip slots.
 *
 * Stubbing as no-op: the existing free list order is preserved. This is
 * safe because the pool is only used for Xbox D3D cache entries which
 * don't exist in our D3D11 shim.
 *
 * Calling convention: cdecl, 0 params (esi = implicit pool pointer)
 */
/*
 * sub_00011000 - "for each of `count` objects `stride` apart, call fnptr(this)"
 *
 * A RenderWare/CRT iteration primitive at the very start of .text. cdecl,
 * ret 16, four args: (start, stride, count, fnptr). The boot spins here at
 * 100% CPU, so `count` is arriving as garbage. This override logs the args and
 * the caller (so the real caller, which the map cannot name, is finally
 * visible) and guards the loop against an absurd count instead of spinning
 * forever. A legitimate per-object loop is never millions of entries.
 */
void sub_00011000(void)
{
    uint32_t start  = MEM32(esp + 4);
    uint32_t stride = MEM32(esp + 8);
    uint32_t count  = MEM32(esp + 12);
    uint32_t fnptr  = MEM32(esp + 16);

    static uint32_t s_calls = 0;
    uint32_t n = ++s_calls;

    if (n <= 12 || count > 0x10000) {
        fprintf(stderr, "  [FOREACH] sub_00011000 #%u: start=0x%08X stride=%u "
                "count=%u fn=0x%08X caller=%p\n",
                n, start, stride, count, fnptr, _ReturnAddress());
        fflush(stderr);
    }

    /* A real object array is not millions of elements; a garbage count is the
     * spin. Cap it so the boot proceeds and the bad caller is visible above,
     * rather than wedging the whole game. */
    if (count > 0x10000)
        count = 0;

    uint32_t obj = start;
    for (uint32_t i = 0; i < count; i++) {
        ecx = obj;                   /* thiscall: this in ecx */
        PUSH32(esp, 0);              /* dummy return address */
        RECOMP_ICALL(fnptr);
        obj += stride;
    }

    esp += 20;   /* ret 16: dummy return (4) + four args (16) */
    return;
}

void sub_001BEFF0(void)
{
    esp += 4;  /* pop dummy return address */
    return;
}

/**
 * sub_001CFDD0 - RW display mode query (STUB)
 *
 * Original: 0x001CFDD0 - 0x001CFE6F (159 bytes, 55 insns)
 * Category: rw_driver_xbox
 * Source: driver/xbox display driver
 *
 * This function queries the Xbox AV system for display timing information
 * (resolution, refresh rate, bytes per scanline, etc.) via kernel calls
 * through thunk entries at 0x36B7E0-0x36B7EC. Since we don't have a real
 * Xbox AV encoder, these calls return 0, and the caller (sub_00021C20)
 * divides by the result → STATUS_INTEGER_DIVIDE_BY_ZERO.
 *
 * The caller uses the return value as a chunk size for display buffer
 * allocation: count = (retval + 0xE9FF) / retval + 2; total = count * retval.
 *
 * Returning 0xEA00 (59904) matches the stripe size the caller already uses
 * for other display segments, giving: count=3, total=179712 bytes.
 *
 * Calling convention: stdcall, 1 param (ret 4)
 *   [esp+4] = parameter (display string pointer)
 * Returns: display buffer size in eax
 */
void sub_001CFDD0(void)
{
    eax = 0xEA00;  /* 59904 = display buffer stripe size */
    esp += 8;      /* ret 4: pop return addr + 1 param */
    return;
}

/* ============================================================
 * RenderWare per-thread last-error storage
 * ============================================================
 * sub_001D1953 and sub_001D1981 use fs:[0x24], fs:[0x28], fs:[0x04]
 * (Xbox KPCR/TEB) to look up the per-thread error slot in the array
 * at 0x41A7D4 and store the error code at +8.
 *
 * The static recompiler drops the FS segment override and walks
 * absolute VAs 0x04/0x24/0x28 instead, which causes a downstream
 * SIGSEGV when the chained dereferences point at unmapped memory.
 *
 * Replace both with a single-slot global since we don't model FS-relative
 * TLS. The error code is rarely consulted in practice and the original
 * Xbox build worked the same way after thread 1 init.
 *
 * sub_001D1953: stdcall, 1 param ([esp+4] = error code)
 * sub_001D1981: stdcall, 1 param ([esp+4] = NTSTATUS); calls
 *               xbox_RtlNtStatusToDosError then sub_001D1953,
 *               returns the converted Win32 error in eax.
 */
extern uint32_t xbox_RtlNtStatusToDosError(uint32_t Status);
static uint32_t g_rw_last_error = 0;

void sub_001D1953(void)
{
    uint32_t err = MEM32(esp + 4);
    g_rw_last_error = err;
    esp += 8;   /* ret 4: return addr + 1 stdcall arg */
}

void sub_001D1981(void)
{
    uint32_t status = MEM32(esp + 4);
    uint32_t dos_err = xbox_RtlNtStatusToDosError(status);
    g_rw_last_error = dos_err;
    eax = dos_err;
    esp += 8;   /* ret 4: return addr + 1 stdcall arg */
}

/* ============================================================
 * sub_00352560 - D3D8LTCG "GetResourceMemorySize" entry point
 * ============================================================
 * One of many entry points into the giant D3D8LTCG state-flush
 * routine (0x00352560-0x00360A54, ~58 KB, ~12,000 insns). When
 * the auto-generated body is left in place, the function walks
 * deep pointer chains in the (uninitialised) D3D device context
 * and returns garbage. Two callers feed that garbage directly
 * into RW heap-alloc:
 *
 *   sub_0003D890 @ 0x3D91F:  call sub_00352560
 *                            push 0x64800000
 *                            push 0x14
 *                            mov edi, eax           ; size!
 *                            call sub_001D2879      ; alloc(20)
 *                            push 0xB7800000
 *                            push edi               ; alloc(garbage!)
 *
 *   sub_00040B90 @ 0x40BD3:  same pattern
 *
 * Either alloc consumes the entire heap (request 178 MB+) and
 * subsequent setup code dereferences NULL and crashes deep in
 * sub_00040B90 / sub_0003D890.
 *
 * Returning a small constant (0x10000 = 64 KB) gives the caller
 * a reasonable buffer to attach to its resource descriptor.
 * Stride and width info read by the caller from out params is
 * left zero, which downstream code already tolerates.
 *
 * Calling convention is non-standard (8 stack args + eax + edx
 * + ecx fast-call). Caller pops the 8 stack args (esp+=0x20) and
 * the function returns 4-byte "ret 0x18" worth, so we adjust
 * esp by 0x1C (ret addr + 24 stack bytes worth of cleanup the
 * gen code would have done in its prologue/epilogue dance).
 *
 * Callers push 8 stack args and DO NOT clean them — sub_001D2879
 * is called immediately after with its own pushes, so sub_00352560
 * must clean the 8 args itself: `ret 0x20` (32 bytes).
 */
void sub_00352560(void)
{
    eax = 0x10000;   /* 64 KB - reasonable resource size */
    esp += 4 + 32;   /* ret 0x20: pop ret addr + 8 stdcall args */
}

/**
 * sub_001D1818 - Thread start routine (RenderWare initialization)
 *
 * This is a mid-function entry point inside sub_001D17DC (0x001D17DC-0x001D18B0).
 * The automatic recompiler treated it as dead code after an early return at
 * 0x001D1815. In reality, this address is passed as the StartRoutine parameter
 * to PsCreateSystemThreadEx, making it the actual game initialization thread.
 *
 * The original x86 code starts with an SEH prologue:
 *   push 0x18; push 0x36BD40; call __SEH_prolog
 *   and [ebp-4], 0
 *   mov eax, fs:[0x28]    ← TLS pointer (translator drops fs: prefix)
 *
 * What it does:
 *   1. Reads the RW engine context from TLS (fs:[0x28])
 *   2. Copies .data sections into RW engine memory
 *   3. Zeroes BSS
 *   4. Calls the real game init callback (via StartContext1 function pointer)
 *   5. Terminates the thread via PsTerminateSystemThread
 *
 * Stack layout at entry (set up by bridge_PsCreateSystemThreadEx):
 *   [esp+0] = dummy return address (0)
 *   [esp+4] = StartContext1 (function pointer to game init callback)
 *   [esp+8] = StartContext2 (parameter to pass to callback)
 *
 * After our synthetic prologue:
 *   [ebp+8]  = StartContext1 (callback function pointer)
 *   [ebp+12] = StartContext2 (callback parameter)
 *   [ebp-4]  = SEH state (0 = in __try, -1 = outside)
 *   [ebp-28] = saved return value from callback
 *   [ebp-32] = RW engine context pointer
 *   [ebp-36] = destination base pointer
 *   [ebp-40] = data section size
 */
void sub_001D1818(void)
{
    uint32_t ebp;

    /* Synthetic prologue - simulate push ebp; mov ebp, esp; sub esp, 48
     * The original x86 has an SEH prologue (push 0x18; push handler;
     * call __SEH_prolog) that sets up the frame. We replicate the
     * resulting layout so ebp-relative accesses work correctly. */
    PUSH32(esp, 0);     /* push ebp (placeholder for saved ebp) */
    ebp = esp;          /* mov ebp, esp */
    esp -= 48;          /* reserve frame space for locals */

    /* --- Original code from address 0x001D1824 (after SEH prolog) --- */

    /* and [ebp-4], 0  →  SEH state = 0 (entering __try block) */
    MEM32(ebp - 4) = 0;

    /* mov eax, fs:[0x28] - Read TLS / RW engine context.
     * The translator drops the fs: prefix, so MEM32(0x28) reads from
     * our fake TIB at Xbox VA 0x28 (populated in xbox_MemoryLayoutInit). */
    eax = MEM32(0x28);
    MEM32(ebp - 32) = eax;

    /* Get destination pointer from RW context */
    edx = MEM32(eax + 0x28);
    edx = edx + 4;
    MEM32(ebp - 36) = edx;
    MEM32(edx - 4) = edx;      /* self-pointer at block start (RW pattern) */

    /* Calculate data section size */
    ebx = MEM32(0x36BF80);     /* end of .data in XBE */
    esi = MEM32(0x36BF7C);     /* start of .data in XBE */
    ebx = ebx - esi;           /* size = end - start */
    MEM32(ebp - 40) = ebx;

    /* Copy data sections: rep movsd + rep movsb
     * Note: XBOX_PTR() translates Xbox VAs to native pointers since
     * the generated code originally used (void*)(uintptr_t)edi which
     * would be wrong on 64-bit (edi holds Xbox VAs, not native addrs). */
    ecx = ebx;
    edi = edx;
    eax = ecx;
    ecx = ecx >> 2;            /* dword count */
    memcpy((void*)XBOX_PTR(edi), (void*)XBOX_PTR(esi), ecx * 4);
    esi += ecx * 4; edi += ecx * 4; ecx = 0;

    ecx = eax;
    ecx = ecx & 3;             /* remaining bytes */
    memcpy((void*)XBOX_PTR(edi), (void*)XBOX_PTR(esi), ecx);
    esi += ecx; edi += ecx; ecx = 0;

    /* Zero BSS section */
    ecx = MEM32(0x36BF8C);     /* BSS size */
    if (TEST_Z(ecx, ecx)) goto loc_001D187D;   /* skip if no BSS */

    /* rep stosd + rep stosb to zero BSS */
    eax = 0;
    edi = ebx + edx;           /* BSS start = data_size + dest_base */
    edx = ecx;
    ecx = ecx >> 2;            /* dword count */
    { uint32_t _i; for (_i = 0; _i < ecx; _i++) MEM32(edi + _i*4) = eax; }
    edi += ecx * 4; ecx = 0;

    ecx = edx;
    ecx = ecx & 3;             /* remaining bytes */
    memset((void*)XBOX_PTR(edi), (uint8_t)eax, ecx);
    edi += ecx; ecx = 0;

loc_001D187D:
    /* Call sub_001D1628(1) - enable debug output */
    PUSH32(esp, 1);
    PUSH32(esp, 0); sub_001D1628();

    /* Call the REAL game init callback: StartContext1(StartContext2)
     * ebp+8 = function pointer, ebp+0xC = parameter */
    PUSH32(esp, MEM32(ebp + 0xC));
    PUSH32(esp, 0); RECOMP_ICALL(MEM32(ebp + 8));

    /* Save callback return value */
    MEM32(ebp - 28) = eax;

    /* Call sub_001D1628(0) - disable debug output */
    PUSH32(esp, 0);
    PUSH32(esp, 0); sub_001D1628();

    goto loc_001D18A2;

    /* SEH exception handler (unreachable in normal flow) */
    PUSH32(esp, MEM32(ebp - 20));
    PUSH32(esp, 0); sub_001D17DC();
    esp += 4; return;

loc_001D18A2:
    /* SEH state = -1 (leaving __try block) */
    MEM32(ebp - 4) = MEM32(ebp - 4) | 0xFFFFFFFFu;

    /* PsTerminateSystemThread(return_value)
     * On real Xbox this doesn't return. In our recompiled version,
     * the bridge stub returns and we clean up the frame. */
    PUSH32(esp, MEM32(ebp - 28));
    PUSH32(esp, 0); RECOMP_ICALL(MEM32(0x36B898));

    /* Clean up frame and return (replaces __debugbreak in generated code) */
    esp = ebp + 4;  /* pop ebp + skip saved ebp */
    return;
}

/**
 * sub_001D2793 - Game initialization callback
 *
 * This is an undetected function in the gap between sub_001D276B (ends ~0x001D278E)
 * and xbe_entry_point (starts 0x001D2807). The recompiler didn't detect it because
 * it's only reached via function pointer - pushed as StartContext1 parameter to
 * PsCreateSystemThreadEx at address 0x001D2852 in xbe_entry_point.
 *
 * Called by sub_001D1818 (thread start routine) via:
 *   RECOMP_ICALL(MEM32(ebp + 8))  where ebp+8 = 0x001D2793
 *
 * What it does:
 *   1. Calls sub_001D3F2F (RenderWare global init)
 *   2. Calls sub_001D2EE5 (engine setup)
 *   3. Reads Xbox KPCR via fs:[0x20] → checks process block at offset 0x250
 *   4. If process block pointer valid, sets up TLS-relative data structure
 *   5. Calls sub_001D3EA2 and sub_001D3E4A (validation/finalization)
 *   6. Calls sub_00156400(0, 0, 0) (cdecl - game subsystem init)
 *   7. Calls sub_001D2E6F(1, 1, 0) (stdcall - enable game systems)
 *   8. Returns 0
 *
 * Uses stdcall: ret 4 (takes 1 parameter from caller - StartContext2)
 *
 * Xbox x86 (0x001D2793-0x001D2806):
 *   call sub_001D3F2F
 *   call sub_001D2EE5
 *   mov eax, fs:[0x20]         ; KPCR from TIB
 *   mov eax, [eax+0x250]       ; process block field
 *   test eax, eax / je skip
 *   mov ecx, [eax+0x24]        ; pointer from process block
 *   ...TLS setup using fs:[0x28], fs:[0x04], [0x41A7D4]...
 *   call sub_001D3EA2
 *   call sub_001D3E4A
 *   push 0/0/0; call sub_00156400; add esp, 0xC
 *   push 0/1/1; call sub_001D2E6F
 *   xor eax, eax; ret 4
 */
void sub_001D2793(void)
{
    uint32_t ebp;

    /* call sub_001D3F2F - RenderWare global init (version/cache check) */
    fprintf(stderr, "  [init] sub_001D3F2F (RW global init)...\n");
    PUSH32(esp, 0); sub_001D3F2F();

    /* call sub_001D2EE5 - engine setup (D3D device, timers, DPCs) */
    fprintf(stderr, "  [init] sub_001D2EE5 (engine setup)...\n");
    PUSH32(esp, 0); sub_001D2EE5();
    fprintf(stderr, "  [init] sub_001D2EE5 done\n");

    /* mov eax, fs:[0x20] - KPCR pointer from fake TIB
     * On Xbox, fs:[0x20] is the KPCR (Kernel Processor Control Region).
     * Our fake TIB at VA 0x20 is initialized to 0 (no KPCR), which
     * causes the code to skip the TLS setup block below. */
    eax = MEM32(0x20);

    /* mov eax, [eax + 0x250] - read from KPCR + 0x250 */
    eax = MEM32(eax + 0x250);

    /* test eax, eax; je loc_001D27B2 */
    if (TEST_Z(eax, eax)) goto loc_001D27B2;

    /* mov ecx, [eax + 0x24] */
    ecx = MEM32(eax + 0x24);

    /* jmp loc_001D27B4 */
    goto loc_001D27B4;

loc_001D27B2:
    /* xor ecx, ecx */
    ecx = 0;

loc_001D27B4:
    /* test ecx, ecx; je loc_001D27DF - skip TLS setup if no pointer */
    if (TEST_Z(ecx, ecx)) goto loc_001D27DF;

    /* push edi (callee-save) */
    PUSH32(esp, edi);

    /* mov eax, fs:[0x28] - TLS array pointer from fake TIB */
    eax = MEM32(0x28);

    /* mov edi, fs:[0x04] - stack base from fake TIB */
    edi = MEM32(0x04);

    /* mov edx, [0x41A7D4] - TLS index for this module */
    edx = MEM32(0x41A7D4);

    /* mov edx, [edi + edx*4] - TLS slot[index] */
    edx = MEM32(edi + edx * 4);

    /* sub edx, [eax + 0x28] - subtract base from RW context */
    edx = edx - MEM32(eax + 0x28);

    /* mov byte [ecx], 1 - set enable flag */
    MEM8(ecx) = 1;

    /* add edx, 8 */
    edx = edx + 8;

    /* mov [ecx + 4], edx - store TLS-relative offset */
    MEM32(ecx + 4) = edx;

    /* pop edi */
    POP32(esp, edi);

loc_001D27DF:
    /* call sub_001D3EA2 - validation/finalization */
    fprintf(stderr, "  [init] sub_001D3EA2 (RW validate)... MEM32(0x754D94)=0x%08X\n", MEM32(0x754D94));
    PUSH32(esp, 0); sub_001D3EA2();
    fprintf(stderr, "  [init] after sub_001D3EA2: MEM32(0x754D94)=0x%08X\n", MEM32(0x754D94));

    /* call sub_001D3E4A - C++ static constructors */
    fprintf(stderr, "  [init] sub_001D3E4A (static init)... MEM32(0x754D94)=0x%08X\n", MEM32(0x754D94));
    PUSH32(esp, 0); sub_001D3E4A();
    fprintf(stderr, "  [init] sub_001D3E4A done\n");

    /* push 0; push 0; push 0; call sub_00156400; add esp, 0xC (cdecl) */
    fprintf(stderr, "  [init] sub_00156400 (game subsystem init)...\n");
    PUSH32(esp, 0);
    PUSH32(esp, 0);
    PUSH32(esp, 0);
    PUSH32(esp, 0); sub_00156400();
    esp += 0xC;  /* cdecl: caller cleans 3 args */

    /* push 0; push 1; push 1; call sub_001D2E6F (stdcall: callee cleans) */
    fprintf(stderr, "  [init] sub_001D2E6F (enable game systems)...\n");
    PUSH32(esp, 0);
    PUSH32(esp, 1);
    PUSH32(esp, 1);
    PUSH32(esp, 0); sub_001D2E6F();

    /* xor eax, eax - return 0 */
    eax = 0;

    /* ret 4 - stdcall: pop return addr + 1 parameter */
    esp += 4;  /* pop dummy return address */
    esp += 4;  /* pop 1 parameter (StartContext2 from caller) */
    return;
}

/**
 * sub_001D5707 - Xbox D3D8 cache initialization (STUB)
 *
 * Original: 0x001D5707 - 0x001D5E66 (1887 bytes, 563 insns)
 * Category: rw_driver_xbox
 * Source: driver/xbox/xbcache.c
 *
 * This function initializes the NV2A GPU texture/surface cache for the Xbox
 * D3D8 library. It reads from internal D3D device fields (offsets +0x1A04,
 * +0x1A08, etc.) that don't exist in our D3D11 shim, producing garbage
 * pointers like 0xFFFFFFF8 (null + struct offset), 0x8DCC5823 (code bytes
 * read as data), 0xEBFD7A2D, 0xCCCCCCCC, etc. These cause ~500
 * SKIP-READ/SKIP-WRITE faults in the VEH handler.
 *
 * Since the NV2A GPU doesn't exist, the cache is meaningless. Stubbing this
 * function eliminates the fault flood and may fix downstream issues caused
 * by corrupted cache state.
 *
 * Calling convention: stdcall, 3 params (ret 12)
 *   [esp+4] = memory pool pointer (first param, returned in eax)
 *   [esp+8] = flags
 *   [esp+C] = size
 * Returns: first param (memory pool pointer) in eax
 */
void sub_001D5707(void)
{
    eax = MEM32(esp + 4);  /* return first param (memory pool ptr) */
    esp += 16;             /* ret 12: pop return addr + 3 params */
    return;
}

/**
 * sub_001D5E82 - Xbox D3D8 cache init variant (STUB)
 *
 * Original: 0x001D5E82 - 0x001D6063 (481 bytes, 158 insns)
 * Category: rw_driver_xbox
 * Source: driver/xbox/xbcache.c
 *
 * Another Xbox D3D8 cache initialization function from the same module as
 * sub_001D5707. Reads from uninitialized D3D device fields, producing
 * garbage pointers (0x8DCC5823 etc.) and bogus heap allocation sizes
 * (357MB, 1.4GB). Stubbed for same reason as sub_001D5707.
 *
 * Calling convention: stdcall, 3 params (ret 12)
 *   [esp+4] = memory pool pointer (returned in eax)
 *   [esp+8] = flags
 *   [esp+C] = size
 * Returns: first param in eax
 */
void sub_001D5E82(void)
{
    eax = MEM32(esp + 4);  /* return first param (memory pool ptr) */
    esp += 16;             /* ret 12: pop return addr + 3 params */
    return;
}

/**
 * sub_001F5810 - Xbox render pipeline begin frame (STUB)
 *
 * Original: 0x001F5810 - 0x001F5834 (36 bytes, 8 insns)
 * Category: rw_world_pipe_xbox
 *
 * "Begin render frame" for the Xbox NV2A render pipeline. Sets global
 * MEM32(0x41B41C) = 1 (pipeline active flag), calls sub_001FBDF0 (init),
 * sub_001F5E30/sub_001F5C40 (pipeline attach), then tail-jumps to
 * sub_001FC3A0 (669 bytes, the main pipeline processor).
 *
 * sub_001FC3A0 triggers a deep call chain (sub_001CAD10 → sub_001CB2D0 →
 * sub_001CD620 etc.) that processes all Xbox render operations. Since we
 * don't have NV2A hardware and all pipeline data is garbage from stubbed
 * D3D cache init, this call chain consumes ~160 bytes of Xbox stack per
 * iteration and overflows the 1MB simulated stack.
 *
 * Stub: no-op. The paired sub_001F5840 (end frame) still increments the
 * frame counter. sub_00135500 loops 30 times calling begin/end frame.
 *
 * Calling convention: cdecl, 0 params
 */
void sub_001F5810(void)
{
    esp += 4;    /* pop dummy return address */
    return;
}

/**
 * sub_001F5840 - Xbox render pipeline end frame (STUB)
 *
 * Original: 0x001F5840 - 0x001F5866 (38 bytes, 10 insns)
 * Category: rw_world_pipe_xbox
 *
 * "End render frame" counterpart to sub_001F5810. Calls sub_001F5E30 +
 * sub_001F5CB0 (pipeline detach, already stubbed), increments frame counter
 * MEM32(0x41B418), clears pipeline active flag MEM32(0x41B41C) = 0.
 *
 * Since sub_001F5810 is stubbed, we only need to increment the frame counter
 * to keep the game's frame tracking consistent.
 *
 * Calling convention: cdecl, 0 params
 */
void sub_001F5840(void)
{
    MEM32(0x41B418) = MEM32(0x41B418) + 1;  /* increment frame counter */
    MEM32(0x41B41C) = 0;                      /* clear pipeline active flag */
    esp += 4;    /* pop dummy return address */
    return;
}

/**
 * sub_001F5C40 - Xbox render pipeline process/attach (STUB)
 *
 * Original: 0x001F5C40 - 0x001F5CA5 (101 bytes, 47 insns)
 * Category: rw_world_pipe_xbox
 *
 * Walks a linked list of RenderWare render pipeline entries starting at
 * (esi + 0xC), calling a callback via vtable offset 0x2C on each entry.
 * The linked list nodes are at offset +20 within each pipeline object.
 *
 * Problem: The linked list head was populated by Xbox D3D cache init
 * (xbcache.c) which is stubbed. The list contains garbage pointers, causing
 * MEM32(esi - 20) to read Xbox VA 0xFFFFFFEC (near-null - 20) in an infinite
 * loop. Accounts for ~49,600 VEH fault-skips.
 *
 * Calling convention: cdecl, 2 params
 *   [esp+4] = pipeline object pointer
 *   [esp+8] = flag (0 or non-zero)
 * Returns: eax = pipeline object pointer or 0 (no valid pipeline)
 * Caller cleans 8 bytes of params.
 */
void sub_001F5C40(void)
{
    eax = 0;     /* no valid pipeline found */
    esp += 4;    /* pop dummy return address */
    return;
}

/**
 * sub_001F5CB0 - Xbox render pipeline process/detach (STUB)
 *
 * Original: 0x001F5CB0 - 0x001F5D15 (101 bytes, 47 insns)
 * Category: rw_world_pipe_xbox
 *
 * Twin of sub_001F5C40 - identical structure, walks same garbage linked list
 * but calls vtable offset 0x30 instead of 0x2C. Same infinite loop problem.
 *
 * Calling convention: cdecl, 2 params. Caller cleans 8 bytes.
 * Returns: eax = pipeline object pointer or 0
 */
void sub_001F5CB0(void)
{
    eax = 0;     /* no valid pipeline found */
    esp += 4;    /* pop dummy return address */
    return;
}

/**
 * sub_0002DDF0 - Pipeline name lookup (STUB)
 *
 * Original: 0x0002DDF0 - 0x0002DE40 (80 bytes, 43 insns)
 * Category: game_engine
 *
 * Searches a RenderWare pipeline table for a named entry by iterating
 * entries at MEM32(esi+0xC) with count MEM32(esi+8), calling sub_00244C51
 * for case-insensitive string comparison on each entry's name at offset 0x48.
 *
 * Problem: esi points to a pipeline table structure populated by Xbox D3D
 * cache init (xbcache.c). Since those init functions are stubbed, the table
 * contains garbage: count is huge, array pointer is ~0x6C000000 (out of
 * range), causing 50,000+ VEH fault-skips as the loop reads progressively
 * further into unmapped memory.
 *
 * Stub returns 0 (no match found). The caller (sub_0002DE40) stores this
 * as the pipeline entry pointer, falling back to a default pipeline.
 *
 * Calling convention: stdcall, 1 param (ret 4)
 *   [esp+4] = Xbox VA of pipeline name string to search for
 * Returns: eax=0 (no match) or eax=pointer to matching entry
 */
void sub_0002DDF0(void)
{
    eax = 0;     /* no match found */
    esp += 8;    /* ret 4: pop return addr + 1 param */
    return;
}

/**
 * sub_00244C51 - Xbox pipeline string comparison (STUB)
 *
 * Original: 0x00244C51 - 0x00244CA0 (79 bytes, 42 insns)
 * Category: rw_world_pipe_xbox
 *
 * Case-insensitive string comparison used during RenderWare render pipeline
 * selection. When the Xbox pipeline table (0x41D4A8) is active, this function
 * compares two pipeline name strings byte-by-byte. One string pointer comes
 * from the Xbox D3D cache which contains garbage from uninitialized NV2A
 * state, causing 50,000+ VEH fault-skip iterations as it reads through
 * code bytes interpreted as data.
 *
 * Both this function AND its fallback (sub_00248FF0, also xbcache.c) read
 * from the same garbage pointers. Stub returns "not equal" (-1) to prevent
 * the Xbox pipeline from being selected. The caller will fall through to
 * the next candidate pipeline (eventually the generic software one).
 *
 * Calling convention: cdecl, 2 params
 *   [esp+4] = string pointer 1 (from D3D cache structure + 0x48)
 *   [esp+8] = string pointer 2 (pipeline name to match)
 * Returns: 0 if equal, non-zero if not equal
 * Caller cleans 8 bytes of params after call.
 */
void sub_00244C51(void)
{
    eax = (uint32_t)-1;  /* not equal - reject Xbox pipeline */
    esp += 4;            /* pop dummy return address */
    return;
}

/**
 * sub_00249B7C - CRT FPU exception handler (with inlined tail jump target)
 *
 * Original x86: 0x00249B7C sets up an EBP frame, copies some args to locals,
 * then tail-jumps (JMP) to 0x00249B9C which continues using the same frame.
 * The auto-recompiler emits this as two separate C functions, but sub_00249B9C
 * has its own local `ebp` which starts uninitialized (0). When it does
 * `MEMD(ebp - 8)`, it writes to Xbox VA 0xFFFFFFF8 → crash.
 *
 * Fix: inline sub_00249B9C's code so both halves share the same `ebp` local.
 *
 * This is the CRT _fltused / __control87 / pow helper chain:
 *   sub_00244E9C → sub_00244EC0 → sub_00249CB9 → sub_00249B7C → sub_0024BC71
 */
void sub_00249B7C(void)
{
    uint32_t ebp;
    double _fp_stack[8];
    int _fp_top = 0;
    #define fp_push_m(v) (_fp_stack[--_fp_top & 7] = (v))
    #define fp_pop_m() (_fp_top++)
    #define fp_popp_m() (fp_pop_m())
    #define fp_top_m() _fp_stack[_fp_top & 7]

    /* sub_00249B7C prologue */
    PUSH32(esp, ebp);
    ebp = esp;
    esp = esp + 0xFFFFFFE0u; /* sub esp, 0x20 */
    MEM32(ebp - 32) = eax;
    eax = MEM32(ebp + 0x18);
    MEM32(ebp - 16) = eax;
    eax = MEM32(ebp + 0x1C);
    MEM32(ebp - 12) = eax;

    /* --- inlined sub_00249B9C (tail jump target) --- */
    MEMD(ebp - 8) = fp_top_m(); fp_popp_m(); /* fstp qword [ebp-8] */
    MEM32(ebp - 28) = ecx;
    eax = MEM32(ebp + 0x10);
    ecx = MEM32(ebp + 0x14);
    MEM32(ebp - 24) = eax;
    MEM32(ebp - 20) = ecx;
    eax = ebp + 8;
    ecx = ebp - 32;
    PUSH32(esp, eax);
    PUSH32(esp, ecx);
    PUSH32(esp, edx);
    PUSH32(esp, 0); sub_0024BC71();

    /* loc_00249BBC */
    esp = esp + 0xC;
    fp_push_m(MEMD(ebp - 8)); /* fld qword [ebp-8] */
    if (!CMP_EQ(MEM16(ebp + 8), 0x27F)) {
        /* fldcw word ptr [ebp + 8] - FPU control word restore (no-op for us) */
    }

    /* leave; ret */
    esp = ebp;
    POP32(esp, ebp);
    esp += 4;
    return;

    #undef fp_push_m
    #undef fp_pop_m
    #undef fp_popp_m
    #undef fp_top_m
}

/**
 * sub_00249B9C - continuation of sub_00249B7C (shared frame)
 *
 * This is the tail jump target of sub_00249B7C. In rare cases it's called
 * directly (not through sub_00249B7C). When called directly, ebp must be
 * inherited from the caller via g_seh_ebp. But typically it's only reached
 * via the tail jump, which we've inlined above. This stub exists so the
 * dispatch table doesn't call the broken generated version.
 */
void sub_00249B9C(void)
{
    uint32_t ebp;
    double _fp_stack[8];
    int _fp_top = 0;
    #define fp_push_m(v) (_fp_stack[--_fp_top & 7] = (v))
    #define fp_pop_m() (_fp_top++)
    #define fp_popp_m() (fp_pop_m())
    #define fp_top_m() _fp_stack[_fp_top & 7]

    /* Inherit ebp from caller - this function expects to share a frame */
    ebp = g_seh_ebp;

    MEMD(ebp - 8) = fp_top_m(); fp_popp_m();
    MEM32(ebp - 28) = ecx;
    eax = MEM32(ebp + 0x10);
    ecx = MEM32(ebp + 0x14);
    MEM32(ebp - 24) = eax;
    MEM32(ebp - 20) = ecx;
    eax = ebp + 8;
    ecx = ebp - 32;
    PUSH32(esp, eax);
    PUSH32(esp, ecx);
    PUSH32(esp, edx);
    PUSH32(esp, 0); sub_0024BC71();

    esp = esp + 0xC;
    fp_push_m(MEMD(ebp - 8));
    if (!CMP_EQ(MEM16(ebp + 8), 0x27F)) {
        /* fldcw - no-op */
    }

    esp = ebp;
    POP32(esp, ebp);
    esp += 4;
    return;

    #undef fp_push_m
    #undef fp_pop_m
    #undef fp_popp_m
    #undef fp_top_m
}

/**
 * NV2A push buffer statistics (shared across push buffer functions)
 */
static volatile uint32_t g_pb_flush_count = 0;
static volatile uint32_t g_pb_kick_count = 0;
static volatile uint32_t g_pb_alloc_count = 0;
static volatile uint32_t g_pb_total_dwords = 0;
static volatile uint32_t g_pb_method_count = 0;

/**
 * nv2a_parse_push_buffer - Parse and log NV2A push buffer commands
 *
 * Reads commands from base..write_ptr, decodes the NV2A command format:
 *   - Increasing methods:     (word & 0xe0030003) == 0
 *   - Non-increasing methods: (word & 0xe0030003) == 0x40000000
 *   - Jump:  (word & 3) == 1
 *   - Call:  (word & 3) == 2
 *   - Return: word == 0x00020000
 */
static void nv2a_parse_push_buffer(uint32_t base, uint32_t write_ptr)
{
    if (write_ptr <= base) return;

    uint32_t num_dwords = (write_ptr - base) / 4;
    g_pb_total_dwords += num_dwords;

    /* Get NV2A state for PGRAPH dispatch */
    extern void *nv2a_get_state(void);
    extern void pgraph_method(void *d, uint32_t subchannel,
                               uint32_t method, uint32_t param);
    void *nv2a = nv2a_get_state();

    /* Parse commands and dispatch to PGRAPH */
    uint32_t pos = base;
    uint32_t method_count_local = 0;
    while (pos < write_ptr) {
        uint32_t word = MEM32(pos);
        pos += 4;

        if ((word & 0xe0030003) == 0) {
            /* increasing methods */
            uint32_t count = (word >> 18) & 0x7ff;
            uint32_t method = word & 0x1ffc;
            uint32_t subchan = (word >> 13) & 7;
            for (uint32_t i = 0; i < count && pos < write_ptr; i++) {
                uint32_t param = MEM32(pos);
                pos += 4;
                method_count_local++;
                if (nv2a) {
                    pgraph_method(nv2a, subchan, method + i * 4, param);
                }
            }
        } else if ((word & 0xe0030003) == 0x40000000) {
            /* non-increasing methods */
            uint32_t count = (word >> 18) & 0x7ff;
            uint32_t method = word & 0x1ffc;
            uint32_t subchan = (word >> 13) & 7;
            for (uint32_t i = 0; i < count && pos < write_ptr; i++) {
                uint32_t param = MEM32(pos);
                pos += 4;
                method_count_local++;
                if (nv2a) {
                    pgraph_method(nv2a, subchan, method, param);
                }
            }
        } else if ((word & 3) == 1) {
            /* jump */
        } else if ((word & 3) == 2) {
            /* call */
        } else if (word == 0x00020000) {
            /* return */
        } else if (word == 0) {
            /* NOP / padding */
        } else {
            /* unknown command */
            break;
        }
    }
    g_pb_method_count += method_count_local;

    /* Log periodically */
    static uint32_t last_log_flush = 0;
    if (g_pb_flush_count <= 5 ||
        g_pb_flush_count - last_log_flush >= 1000) {
        fprintf(stderr, "  [PB] flush #%u: %u dwords, %u methods (total: %u dwords, %u methods)\n",
                g_pb_flush_count, num_dwords, method_count_local,
                g_pb_total_dwords, g_pb_method_count);
        fflush(stderr);
        last_log_flush = g_pb_flush_count;
    }
}

/**
 * sub_00351A20 - D3D8 push buffer flush
 *
 * On Xbox, this submits the current push buffer contents to the NV2A GPU
 * via DMA and resets the write pointer for the next batch of commands.
 *
 * NV2A-aware: parses and logs push buffer commands before reset.
 *
 * The buffer base is stored at Xbox VA 0x35D69C (set during init in main.c).
 * The write pointer is at 0x35D6A0, end pointer at 0x35D6A4.
 *
 * Calling convention: cdecl, no args.
 * Called as: PUSH32(esp, 0); sub_00351A20();
 */
void sub_00351A20(void)
{
    uint32_t base = MEM32(0x35D69C);
    uint32_t write_ptr = MEM32(0x35D6A0);

    g_pb_flush_count++;

    /* Parse push buffer commands before discarding */
    if (write_ptr > base) {
        nv2a_parse_push_buffer(base, write_ptr);
    }

    /* Reset write pointer to buffer base */
    MEM32(0x35D6A0) = base;

    /* Pop dummy return address (simulated x86 'ret') */
    esp += 4;
    return;
}


/**
 * sub_003518E0 - NV2A push buffer kickoff
 *
 * Original: 0x003518E0 (D3D8LTCG section)
 *
 * On Xbox, this is the push buffer "kick" function that submits queued
 * GPU commands to the NV2A via DMA. It updates the PUT pointer, waits
 * for the GPU GET pointer to advance, and handles ring buffer wrapping.
 *
 * NV2A-aware: logs kick events. In the future, this will write the PUT
 * pointer to PFIFO registers to trigger command processing.
 *
 * Calling convention: ret 8 (2 params on stack).
 */
extern void parse_live_pushbuffer(void);

void sub_003518E0(void)
{
    g_pb_kick_count++;

    /* Simulate GPU consuming all push buffer commands by resetting
     * the write cursor back to the base. This prevents the gen code
     * from spinning in a PB-full check (device[0] >= device[4]).
     * The gen code checks device[0] < device[4] for space. */
    uint32_t base = MEM32(0x35D69C);
    uint32_t dev = MEM32(0x35FB48);
    if (dev >= 0x4000000) dev = dev % 0x4000000;

    /* Parse PB commands written since last reset BEFORE resetting.
     * Cap at ring size to avoid processing garbage from cursor overflow. */
    uint32_t write_ptr = (dev != 0 && dev < 0x4000000) ? MEM32(dev) : MEM32(0x35D6A0);
    uint32_t ring_size = (dev != 0 && dev < 0x4000000) ? MEM32(dev + 0x44) : 0x400000;
    if (write_ptr > base && (write_ptr - base) <= ring_size) {
        uint32_t bytes = write_ptr - base;
        static uint32_t total_pb_bytes = 0;
        total_pb_bytes += bytes;
        if (g_pb_kick_count <= 10 || (g_pb_kick_count % 1000) == 0)
            fprintf(stderr, "  [PB-KICK] #%u: %u bytes, total=%u\n",
                    g_pb_kick_count, bytes, total_pb_bytes);
        /* Parse and translate to D3D11 */
        MEM32(0x35D6A0) = write_ptr;  /* sync global write ptr for parser */
        parse_live_pushbuffer();
    } else if (write_ptr > base && g_pb_kick_count <= 5) {
        fprintf(stderr, "  [PB-KICK] #%u: OVERFLOW! write=0x%X base=0x%X delta=%u (ring=%u)\n",
                g_pb_kick_count, write_ptr, base, write_ptr - base, ring_size);
    }

    /* Reset cursors — simulate GPU consumed everything */
    MEM32(0x35D6A0) = base;
    if (dev != 0 && dev < 0x4000000) {
        MEM32(dev + 0x00) = base;  /* device PB cursor = base */
        MEM32(dev + 0x04) = base + MEM32(dev + 0x44);  /* segment limit = end */
        /* Keep fake GPU read at max so fence waits always exit */
        MEM32(dev + 0x3004) = 0xFFFFFFFFu;
    }

    eax = base;  /* return base as new position */
    esp += 12;   /* ret 8 */
    return;
}


/**
 * sub_00351770 - NV2A push buffer space allocation
 *
 * Original: 0x00351770 (D3D8LTCG section)
 *
 * On Xbox, this allocates space in the push buffer ring, potentially
 * triggering a kickoff (sub_003518E0) if insufficient space remains.
 *
 * NV2A-aware: returns the current write pointer so D3D8 functions
 * can actually write push buffer commands. Auto-flushes when near
 * the end of the buffer.
 *
 * Calling convention: ret 4 (1 param on stack).
 * Returns eax = allocated push buffer address.
 */
void sub_00351770(void)
{
    uint32_t requested_dwords = MEM32(esp + 4); /* param: dwords needed */
    uint32_t write_ptr = MEM32(0x35D6A0);
    uint32_t end_ptr = MEM32(0x35D6A4);
    uint32_t base = MEM32(0x35D69C);

    g_pb_alloc_count++;

    /* Check if we have space */
    uint32_t remaining = (end_ptr - write_ptr) / 4;
    if (remaining < requested_dwords + 16) {
        /* Near end of buffer - flush and reset */
        if (write_ptr > base) {
            g_pb_flush_count++;
            nv2a_parse_push_buffer(base, write_ptr);
        }
        MEM32(0x35D6A0) = base;
        write_ptr = base;
    }

    eax = write_ptr;  /* return allocated address */
    esp += 8;     /* ret 4: pop return addr (4) + 1 param (4) */
    return;
}


#if 0 /* gen sub_0034CBF0 re-enabled with PB cursor reset in sub_003518E0 */
/**
 * sub_0034CBF0 - D3D8LTCG render target setup + dirty flag init (OVERRIDE)
 *
 * Original: 0x0034CBF0 - 0x00360A54 (81508 bytes, 19561 insns)
 * CC: cdecl, 2 params (RT surface ptr, DS surface ptr), ret 12
 *
 * Sets up render targets and marks them dirty so sub_00355F50 (the dirty
 * flag processor, now generated) will flush state to the push buffer.
 *
 * Original does:
 *   1. Read device+0x1A04/1A08 (current RT/DS)
 *   2. Mark surfaces dirty: surface[0] += 0x80000
 *   3. Swap render targets
 *   4. Call sub_0034C800, sub_00352040, sub_0034CA10 (device ptr chains)
 *   5. Set global dirty flags at MEM32(0x35FB50)
 *   6. Fall through to render state flush (same code as sub_0034D530)
 *
 * Manual override: sets dirty flags without walking device pointer chains.
 */
void sub_0034CBF0(void)
{
    uint32_t dev_raw = MEM32(0x35FB48);
    /* Convert mirror/native address to Xbox VA (mirrors repeat every 64MB) */
    uint32_t dev = dev_raw;
    if (dev >= 0x4000000) dev = dev % 0x4000000;

    uint32_t rt_param = MEM32(esp + 4);
    uint32_t ds_param = MEM32(esp + 8);

    /* If RT param is 0, use current RT from device.
     * Force xemu snapshot addresses if device got cleared. */
    uint32_t rt = rt_param;
    if (rt == 0 && dev != 0 && dev < 0x4000000) {
        rt = MEM32(dev + 0x1A04);
        if (rt >= 0x4000000) rt = rt % 0x4000000;  /* mirror fix */
        if (rt == 0) rt = 0x0035F0C4;  /* xemu snapshot RT surface */
    }

    /* If DS param is 0, use current DS from device */
    uint32_t ds = ds_param;
    if (ds == 0 && dev != 0 && dev < 0x4000000) {
        ds = MEM32(dev + 0x1A08);
        if (ds >= 0x4000000) ds = ds % 0x4000000;  /* mirror fix */
        if (ds == 0) ds = 0x0035F10C;  /* xemu snapshot DS surface */
    }

    /* Mark RT surface dirty (+0x80000) — matches gen code at loc_0034CC36 */
    if (rt != 0 && rt < 0x4000000) {
        MEM32(rt) = MEM32(rt) + 0x80000;
        /* Also mark linked surface at +0x14 if present */
        uint32_t linked = MEM32(rt + 0x14);
        if (linked != 0 && linked < 0x4000000)
            MEM32(linked) = MEM32(linked) + 0x80000;
    }

    /* Set new RT in device context */
    if (dev != 0 && dev < 0x4000000)
        MEM32(dev + 0x1A04) = rt;

    /* Mark DS surface dirty — matches gen code at loc_0034CCBB */
    if (ds != 0 && ds < 0x4000000) {
        MEM32(ds) = MEM32(ds) + 0x80000;
        uint32_t linked_ds = MEM32(ds + 0x14);
        if (linked_ds != 0 && linked_ds < 0x4000000)
            MEM32(linked_ds) = MEM32(linked_ds) + 0x80000;
    }

    /* Set new DS in device context */
    if (dev != 0 && dev < 0x4000000)
        MEM32(dev + 0x1A08) = ds;

    /* Set global dirty flags — from xemu snapshot: 0x00FF1050
     * This tells sub_00355F50 which state to flush to push buffer.
     * Key bits: viewport, transforms, textures, render states */
    MEM32(0x35FB50) = MEM32(0x35FB50) | 0x00FF1050;

    static uint32_t call_count = 0;
    call_count++;
    if (call_count <= 10 || (call_count % 5000) == 0)
        fprintf(stderr, "  [CBF0] #%u: dev=0x%X rt=0x%X ds=0x%X 1A04=0x%X 1A08=0x%X dirty=0x%08X\n",
                call_count, dev, rt, ds,
                (dev != 0 && dev < 0x4000000) ? MEM32(dev + 0x1A04) : 0,
                (dev != 0 && dev < 0x4000000) ? MEM32(dev + 0x1A08) : 0,
                MEM32(0x35FB50));

    esp += 16; return; /* ret 12: pop ret + 2 params */
}
#endif /* gen sub_0034CBF0 re-enabled */


/**
 * D3D8LTCG render state flush — mid-function entry point stubs
 *
 * The D3D8LTCG library has ONE giant function (0x0034D410-0x00360A54, ~80K)
 * with multiple entry points depending on which dirty flags are set:
 *   sub_0034D410 (top)    — 79K, cdecl 0 params, ret 8 (2 stack params)
 *   sub_0034D530           — 79K, our live push buffer override (ret 12)
 *   sub_0034F5B0           — 70K, cdecl 0 params, called from sub_0003FEE0
 *   sub_003558A0           — 45K, cdecl 0 params, returns new PB position
 *
 * These are called by sub_0003FEE0 and other RW display pipeline functions.
 * On Xbox, they flush accumulated D3D state changes to NV2A push buffer.
 * In our environment, we handle push buffer output in sub_0034D530, so
 * these mid-entry stubs just return the current push buffer position.
 */
/* sub_0034CEF0 in gen code (re-enabled with sub_0034CBF0) */

void sub_0034F5B0(void)
{
    eax = MEM32(0x35D6A0);  /* current write pointer */
    esp += 4; return;
}

void sub_003558A0(void)
{
    eax = MEM32(0x35D6A0);  /* current write pointer */
    esp += 4; return;
}

void sub_0034D410(void)
{
    eax = MEM32(0x35D6A0);
    esp += 12; return;  /* ret 8: pop ret + 2 params */
}


/**
 * sub_0003FEE0 - RW frame render function (MANUAL OVERRIDE)
 *
 * Original: 0x0003FEE0 - 0x000402C0 (992 bytes, 234 insns)
 * CC: stdcall, 2 params (game_obj at [ebp+8], scene_desc at [ebp+C])
 *
 * Uses typed structs from rw_structs.h / rw_d3d_device.h for offset mapping:
 *   game_obj  → RwGameRenderContext (0x4D6170)
 *   dev       → XboxD3DDevice (0x35D6A0)
 *
 * Pipeline steps:
 *   1-3. D3D state flush — SKIP (stubs handle sub_0034F5B0/sub_003558A0)
 *   4.   Matrix copy: sub_0003FE10 (src_matrices → dst_matrices)
 *   5.   Scene descriptor → scene_desc[4]
 *   6.   D3D render state — SKIP
 *   7.   device.render_state_matrix → ctx.device_state
 *   8.   sub_00040CF0 (rotation matrix builder) — RE-ENABLED with init'd state
 *   9.   Matrix ops (sub_001AF280, sub_001CF153, sub_00040310) — SKIP (need
 *        proper camera data from sub_00040CF0 output first)
 *  10.   D3D clear via sub_0034C2E0
 *  11.   Render dispatch: sub_001AD350 × 3 passes
 *  12.   Post-render cleanup
 */
void sub_0003FE10(void);  /* matrix copy helper: edx=src(+0x500), eax=dst(+0x6E0) */
void sub_00040CF0(void);  /* rotation/camera matrix builder (re-enabled) */
void sub_0003FEE0(void)
{
    static uint32_t call_count = 0;
    call_count++;
    int log = (call_count <= 10 || (call_count % 5000) == 0);

    /* stdcall frame: push ebp, mov ebp esp */
    uint32_t saved_ebp;
    PUSH32(esp, saved_ebp);
    uint32_t frame_ebp = esp;

    /* Read params — typed via RwGameRenderContext / XboxD3DDevice */
    uint32_t game_obj   = MEM32(frame_ebp + 8);   /* → RwGameRenderContext at 0x4D6170 */
    uint32_t scene_desc = MEM32(frame_ebp + 0xC);  /* scene descriptor (or zeroed) */
    uint32_t dev = MEM32(0x35FB48);                /* → XboxD3DDevice at 0x35D6A0 */

    if (log)
        fprintf(stderr, "  [FEE0] #%u: ctx=0x%X scene=0x%X dev=0x%X\n",
                call_count, game_obj, scene_desc, dev);

    /* ── Step 1-3: D3D state flush — SKIP (stubs return PB position) ── */

    /* ── Step 4: sub_0003FE10 — copy src_matrices → dst_matrices ──
     * RwGameRenderContext: +0x500 (src_matrices[4]) → +0x6E0 (dst_matrices[4])
     * sub_0003FE10 reads edx=src, eax=dst (register calling convention). */
    if (game_obj != 0 && game_obj < 0x4000000) {
        edx = game_obj + 0x500;  /* offsetof(RwGameRenderContext, src_matrices) */
        eax = game_obj + 0x6E0;  /* offsetof(RwGameRenderContext, dst_matrices) */
        PUSH32(esp, 0); sub_0003FE10();
    }

    /* ── Step 5: Copy scene descriptor → ctx.scene_desc[4] ──
     * movaps 16 bytes, then advance animation counter at +0x664. */
    if (scene_desc != 0 && scene_desc < 0x4000000 && game_obj != 0) {
        /* RwGameRenderContext::scene_desc at +0x660 */
        MEM32(game_obj + 0x660) = MEM32(scene_desc);
        MEM32(game_obj + 0x664) = MEM32(scene_desc + 4);
        MEM32(game_obj + 0x668) = MEM32(scene_desc + 8);
        MEM32(game_obj + 0x66C) = MEM32(scene_desc + 12);
        /* scene_desc[1] += anim_step (XBOX_ANIM_STEP_VA = 0x3B1684) */
        MEMF(game_obj + 0x664) = MEMF(game_obj + 0x664) + MEMF(0x3B1684);
    }

    /* ── Step 6: D3D render state flush — SKIP ── */

    /* ── Step 7: Copy device.render_state_matrix → ctx.device_state ──
     * XboxD3DDevice::render_state_matrix (+0xCA0) → RwGameRenderContext::device_state (+0x540)
     * Gen code: "rep movsd ecx=0x10" = 64 bytes */
    if (dev != 0 && dev < 0x4000000 && game_obj != 0) {
        memcpy((void*)XBOX_PTR(game_obj + 0x540),  /* ctx.device_state */
               (void*)XBOX_PTR(dev + 0xCA0),       /* dev.render_state_matrix */
               64);
    }

    /* ── Step 7b: Set render viewport / scale / flags ──
     * RwGameRenderContext fields from gen code analysis */
    if (game_obj != 0 && game_obj < 0x4000000) {
        float f_scale = MEMF(0x3B168C);  /* XBOX_SCALE_CONST_VA = 1.0f */
        MEM32(game_obj + 0x998) = 0x80;  /* ctx.viewport_w */
        MEM32(game_obj + 0x99C) = 0x80;  /* ctx.viewport_h */
        MEM32(game_obj + 0x990) = 0;     /* ctx.viewport_x */
        MEM32(game_obj + 0x994) = 0;     /* ctx.viewport_y */
        MEMF(game_obj + 0x9A0) = f_scale; /* ctx.scale_x */
        MEMF(game_obj + 0x9A4) = f_scale; /* ctx.scale_y */
        MEM32(game_obj + 0x9C4) = 0x901; /* ctx.render_flags */
    }

    /* ── Step 8: sub_00040CF0 — rotation/camera matrix builder ──
     * Now safe to call: rw_state_init() populated identity matrices
     * in device.transform_cache, device.render_state_matrix, and
     * all game render context matrix slots.
     *
     * Gen code context:
     *   ecx = esp+0x1C (ptr to ctx.scene_desc = game_obj+0x660)
     *   edx = esp+0x20 (ptr to ctx.work_matrix_3 = game_obj+0x680 area)
     *   eax = esp+0x18 (loop counter, starts at 0)
     *   Stack params: scene_desc ptr, 0x680 ptr */
    if (game_obj != 0 && game_obj < 0x4000000) {
        ecx = game_obj + 0x660;  /* scene_desc ptr */
        edx = game_obj + 0x680;  /* work_matrix_3 */
        eax = 0;                  /* loop counter */
        PUSH32(esp, game_obj + 0x660);
        PUSH32(esp, game_obj + 0x680);
        PUSH32(esp, 0); sub_00040CF0();
    }

    /* ── Step 9: Matrix ops — SKIP for now ──
     * sub_001AF280 (projection builder) needs camera+frustum data
     * sub_001CF153 (matrix multiply) needs valid input from sub_001AF280
     * sub_00040310 (matrix→render state) needs sub_001CF153 output
     * TODO: re-enable once sub_00040CF0 output is validated */

    /* BeginScene for FEE0's geometry draws (bridge already did EndScene) */
    {
        IDirect3DDevice8 *d3d = xbox_GetD3DDevice();
        if (d3d) d3d->lpVtbl->BeginScene(d3d);
    }

    /* ── Step 10: D3D clear — depth only (keep menu content from bridge) ── */
    PUSH32(esp, 0);            /* stencil */
    PUSH32(esp, 0x3F800000);   /* depth = 1.0f */
    PUSH32(esp, 0);            /* color = black (unused — not clearing color) */
    PUSH32(esp, 0x02);         /* flags = depth only (skip color clear) */
    PUSH32(esp, 0);            /* rect count */
    PUSH32(esp, 0);            /* rects ptr */
    PUSH32(esp, 0); sub_0034C2E0();

    /* ── Step 11: Render dispatch — sub_001AD350 × 3 passes ── */
    uint32_t render_base = 0x7397B0;  /* base_obj + 0x12ADB0 = 0x60EA00 + 0x12ADB0 */

    /* Dump the resource structure that sub_001AD350 reads */
    if (log) {
        uint32_t rb4 = MEM32(render_base + 4);
        fprintf(stderr, "  [FEE0] render_base=0x%X [+4]=0x%X\n", render_base, rb4);
        if (rb4 > 0x10000 && rb4 < 0x4000000) {
            fprintf(stderr, "  [FEE0] res: [+0]=0x%X [+4]=0x%X [+8]=0x%X [+14]=0x%X [+15]=0x%X [+1C]=0x%X [+24]=0x%X\n",
                    MEM32(rb4), MEM32(rb4+4), MEM32(rb4+8),
                    (uint32_t)MEM8(rb4+0x14), (uint32_t)MEM8(rb4+0x15),
                    (uint32_t)(int16_t)MEM16(rb4+0x1C), MEM32(rb4+0x24));
        }
    }

    /* Render dispatch: sub_001AD350 × 3 passes.
     * Pass 0, 1: typically 0 entries. Pass 2: 14 entries from static.dat.
     * g_current_geom_base is set by sub_001AD350's inner loop for sub_001D7D10. */
    extern uint32_t g_current_geom_base;
    if (log) fprintf(stderr, "  [FEE0] Pass 0...\n");
    PUSH32(esp, 0);
    PUSH32(esp, render_base);
    PUSH32(esp, 0); sub_001AD350();

    if (log) fprintf(stderr, "  [FEE0] Pass 1...\n");
    PUSH32(esp, 1);
    PUSH32(esp, render_base);
    PUSH32(esp, 0); sub_001AD350();

    if (log) fprintf(stderr, "  [FEE0] Pass 2 (%d entries)...\n",
            (int)(int16_t)MEM16(MEM32(render_base + 4) + 0x1C));
    PUSH32(esp, 2);
    PUSH32(esp, render_base);
    PUSH32(esp, 0); sub_001AD350();
    if (log) fprintf(stderr, "  [FEE0] Pass 2 DONE\n");

    /* EndScene to match BeginScene above */
    {
        IDirect3DDevice8 *d3d = xbox_GetD3DDevice();
        if (d3d) d3d->lpVtbl->EndScene(d3d);
    }

    if (log)
        fprintf(stderr, "  [FEE0] #%u DONE (sub_00040CF0 enabled)\n", call_count);

    /* ── Epilog: restore frame, ret 8 ── */
    esp = frame_ebp;
    POP32(esp, saved_ebp);
    esp += 12; return;  /* ret 8: pop ret + 2 params */
}


/**
 * sub_001C1670 - RW frame matrix propagation (depth-limited)
 *
 * Original: 0x001C1670 - 0x001C173A (202 bytes, 56 insns)
 * CC: stdcall, 1 param (parent LTM pointer via stack), ecx = frame node
 * Frame: EBP-based (0 bytes locals, 24 bytes aligned scratch)
 *
 * Recursively propagates local-to-world matrix through the RW scene graph.
 * Reads parent LTM from stack arg, multiplies with local transform at ecx,
 * stores result at ecx+0x20, then recurses into children linked at ecx+0x44.
 *
 * Manual override: adds recursion depth limiting and pointer validation.
 * Without this, uninitialized scene graph nodes contain non-zero garbage
 * child pointers in valid Xbox RAM, causing unbounded recursion and
 * native stack overflow.
 */
static int s_frame_propagate_depth = 0;
#define MAX_FRAME_DEPTH 64
#define VALID_XBOX_PTR(p) ((p) >= 0x10000u && (p) < 0x04000000u)

void sub_001C1670(void)
{
    uint32_t ebp;
    int _flags = 0;
    float xmm0, xmm1;

    /* Standard EBP-based prologue */
    PUSH32(esp, ebp);
    ebp = esp;
    esp = esp & 0xFFFFFFF0u;
    esp = esp - 0x18;
    PUSH32(esp, esi);
    PUSH32(esp, edi);

    eax = MEM32(ebp + 8); /* parent LTM pointer (arg) */

    /* Validate input pointers */
    if (!VALID_XBOX_PTR(eax) || !VALID_XBOX_PTR(ecx)) {
        goto done;
    }

    /* --- Matrix math (identical to generated code) --- */
    xmm0 = MEMF(eax + 8);
    xmm0 = xmm0 * MEMF(ecx);
    MEMF(esp + 0x10) = xmm0;
    xmm0 = MEMF(eax + 0xC);
    xmm0 = xmm0 * MEMF(ecx + 4);
    edx = MEM32(esp + 0x10);
    MEMF(esp + 0x14) = xmm0;
    xmm0 = MEMF(eax);
    MEM32(esp + 0x18) = edx;
    xmm0 = xmm0 + MEMF(esp + 0x18);
    edx = MEM32(esp + 0x14);
    MEMF(esp + 0x10) = xmm0;
    xmm0 = MEMF(eax + 4);
    MEM32(esp + 0x1C) = edx;
    xmm0 = xmm0 + MEMF(esp + 0x1C);
    edx = MEM32(esp + 0x10);
    MEM32(ecx + 0x20) = edx;
    MEMF(esp + 0x14) = xmm0;
    edx = MEM32(esp + 0x14);
    MEM32(ecx + 0x24) = edx;
    xmm0 = MEMF(ecx + 8);
    xmm0 = xmm0 * MEMF(eax + 8);
    edi = ecx + 0x20;
    MEMF(esp + 0x18) = xmm0;
    xmm0 = MEMF(eax + 0xC);
    xmm0 = xmm0 * MEMF(ecx + 0xC);
    edx = MEM32(esp + 0x18);
    MEMF(esp + 0x1C) = xmm0;
    xmm0 = MEMF(ecx + 0x10);
    MEM32(ecx + 0x28) = edx;
    edx = MEM32(esp + 0x1C);
    MEM32(ecx + 0x2C) = edx;
    xmm1 = MEMF(eax + 0x10);
    /* mulps: xmm0 *= xmm1 (packed 4xfloat - scalar approximation) */
    xmm0 = xmm0 * xmm1;
    MEMF(ecx + 0x30) = xmm0;

    /* --- Recurse into children (depth-limited) --- */
    esi = MEM32(ecx + 0x44);
    if (esi == 0) goto done;

    if (s_frame_propagate_depth >= MAX_FRAME_DEPTH) goto done;
    s_frame_propagate_depth++;

    while (esi != 0) {
        if (!VALID_XBOX_PTR(esi)) break;

        PUSH32(esp, edi);
        ecx = esi + 8;
        PUSH32(esp, 0); sub_001C1670();

        esi = MEM32(esi);  /* next sibling */
    }

    s_frame_propagate_depth--;

done:
    POP32(esp, edi);
    POP32(esp, esi);
    esp = ebp;
    POP32(esp, ebp);
    esp += 8; return; /* ret 4 */
}


/**
 * sub_001D7180 - RW Xbox display driver rendering function (STUB)
 *
 * Original: 0x001D7180 - 0x001D7D10 (2960 bytes, 811 insns)
 * Category: rw_driver_xbox
 * CC: cdecl, 0 params
 *
 * Large Xbox-specific rendering function in the RW display driver.
 * Iterates over D3D internal structures (vertex buffers, texture
 * descriptors, render states) setting up hardware-specific state.
 * These structures are garbage in our D3D11 shim, causing 50K+
 * VEH fault-skip events per frame from reads of addresses in the
 * 0x920xxxxx range (unmapped Xbox VA).
 *
 * Stubbed as no-op since all rendering goes through D3D11.
 */
void sub_001D7180(void)
{
    esp += 4; return; /* ret */
}

/**
 * sub_001D7857 - Mid-function entry into sub_001D7180 (STUB)
 * sub_001D7876 - Mid-function entry into sub_001D7180 (STUB)
 *
 * These are alternate entry points into the same RW Xbox rendering
 * function. Both are cdecl, 0 params, fpo_leaf. Same stub treatment.
 */
void sub_001D7857(void)
{
    esp += 4; return; /* ret */
}

void sub_001D7876(void)
{
    esp += 4; return; /* ret */
}

/**
 * sub_001D7D90 - RW Xbox display driver rendering function #2 (STUB)
 *
 * Original: 0x001D7D90 - 0x001D88D7 (2887 bytes, 661 insns)
 * Category: rw_driver_xbox
 * CC: cdecl, 0 params
 *
 * Second large Xbox rendering function, processes vertex/texture state.
 * Same stubbing rationale as sub_001D7180.
 */
void sub_001D7D90(void)
{
    esp += 4; return; /* ret */
}

/**
 * sub_001D88E0 - RW Xbox world pipeline rendering #1 (STUB)
 * sub_001D8A80 - RW Xbox world pipeline rendering #2 (STUB)
 * sub_001D8FA0 - RW Xbox world pipeline rendering #3 (STUB)
 *
 * Original: 0x001D88E0-0x001D9130 (rw_world_pipe_xbox category)
 * All cdecl, 0 params. These iterate over D3D vertex/texture structures
 * producing 500K+ VEH fault-skips per second from unmapped addresses.
 * Stubbed since all rendering goes through D3D11.
 */
void sub_001D88E0(void)
{
    esp += 4; return; /* ret */
}

void sub_001D8A80(void)
{
    esp += 4; return; /* ret */
}

void sub_001D8FA0(void)
{
    esp += 4; return; /* ret */
}

/**
 * sub_001D93AF - RW Xbox rendering pipeline dispatch (STUB)
 *
 * Original: 0x001D93AF - 0x001D9420 (113 bytes, 38 insns)
 * CC: cdecl, 0 params
 *
 * Top-level dispatch for Xbox rendering pipeline. Calls into
 * sub_001D9230 and related functions that iterate over D3D
 * vertex/texture structures, producing millions of VEH faults.
 */
/**
 * sub_001D9180..sub_001D9360 - RW Xbox rendering pipeline cluster (STUBS)
 *
 * Nine functions (0x001D9180, 0x001D91B0, 0x001D91F0, 0x001D9230,
 * 0x001D9280, 0x001D9290, 0x001D92A0, 0x001D92EF, 0x001D9360)
 * All rw_world_pipe_xbox category, cdecl 0 params.
 *
 * These form a cluster of rendering pipeline helper functions that
 * read/write D3D vertex/texture structures. sub_001D9230 is the
 * hottest function, called from multiple paths and producing millions
 * of VEH fault-skips per frame.
 */
void sub_001D9180(void) { esp += 4; return; }
void sub_001D91B0(void) { esp += 4; return; }
void sub_001D91F0(void) { esp += 4; return; }
void sub_001D9230(void) { esp += 4; return; }
void sub_001D9280(void) {
    /* Frontend prep: indirect tail call through vtable at [param + 0x1C].
     * Original: 11 bytes at 0x001D9280, just "mov eax,[esp+4]; jmp [eax+0x1C]"
     * Called with 1 stack param = object pointer from MEM32(MEM32(0x4D6520)+0x58).
     * Tail-calls the object's render/update method. */
    uint32_t obj = MEM32(esp + 4);
    static uint32_t call_count = 0;
    call_count++;

    if (call_count <= 10 || (call_count % 1000) == 0) {
        uint32_t dev = MEM32(0x4D6520);
        uint32_t sub = (dev > 0x10000 && dev < 0x4000000) ? MEM32(dev + 0x58) : 0xDEAD;
        fprintf(stderr, "  [FE-PREP] sub_001D9280 #%u: [0x4D6520]=0x%08X [+0x58]=0x%08X obj=0x%08X\n",
                call_count, dev, sub, obj);
    }

    if (obj == 0) {
        esp += 4; return;
    }

    /* Object might be native pointer - convert to Xbox VA */
    uint32_t obj_va = obj;
    if (obj_va > 0x4000000) {
        extern ptrdiff_t g_xbox_mem_offset;
        if (g_xbox_mem_offset > 0) {
            uint32_t off32 = (uint32_t)g_xbox_mem_offset;
            if (obj_va >= off32)
                obj_va = (obj_va - off32) % 0x04000000u;
        }
    }

    /* Read vtable method at offset 0x1C */
    uint32_t vtable_target = 0;
    if (obj_va > 0x10000 && obj_va < 0x4000000) {
        vtable_target = MEM32(obj_va + 0x1C);
    } else if (obj > 0x10000) {
        /* Try with original (possibly native) pointer directly */
        vtable_target = MEM32(obj + 0x1C);
    }

    if (call_count <= 10 || (call_count % 1000) == 0)
        fprintf(stderr, "  [FE-PREP] sub_001D9280 #%u: obj=0x%08X (va=0x%08X) vtable[0x1C]=0x%08X\n",
                call_count, obj, obj_va, vtable_target);

    if (vtable_target > 0x10000 && vtable_target < 0x400000) {
        /* Valid Xbox VA - do the tail call */
        eax = obj;
        MEM32(esp + 4) = obj;  /* matches gen code: mov [esp+4], eax */
        RECOMP_ITAIL(vtable_target);
    }

    esp += 4; return;
}
void sub_001D9290(void) { esp += 4; return; }
void sub_001D92A0(void) { esp += 4; return; }
void sub_001D92EF(void) { esp += 4; return; }
void sub_001D9360(void) { esp += 4; return; }

void sub_001D93AF(void)
{
    esp += 4; return; /* ret */
}

/*
 * sub_001D9420 - RW Xbox display driver (rw_driver_xbox) rendering helper
 * sub_001D9450 - RW Xbox world pipeline (rw_world_pipe_xbox) viewport setup
 * sub_001D94A0 - RW Xbox display driver (rw_driver_xbox) rendering submit
 * sub_001D94D0 - RW Xbox world pipeline (rw_world_pipe_xbox) hot rendering path
 *
 * These four functions complete the Xbox rendering pipeline cluster.
 * sub_001D94D0 was the hottest remaining function producing millions
 * of VEH fault-skips per frame via indirect calls into D3D structures.
 */
volatile uint32_t g_present_count = 0;
void sub_001D9420(void) {
    /* RW driver "Present" - calls sub_001DE7E0 (display submit).
     * sub_001DE7E0 does:
     *   1. sub_001E1CD0 (RW render queue management)
     *   2. ICALL([0x759338]) = RW render callback with 3 params
     *
     * Lazy-init: RW init code runs after our early init and clears the
     * function pointer table. Re-populate it on first call if needed. */
    static int table_inited = 0;

    g_present_count++;

    /* Lazy-init the RW display driver function pointer table */
    if (!table_inited || MEM32(0x759338) == 0) {
        rw_init_display_driver_table();
        table_inited = 1;
    }

    if (g_present_count <= 5 || (g_present_count % 1000) == 0) {
        uint32_t rw_cb = MEM32(0x759338);
        uint32_t im2d_fn = MEM32(0x7592B4);
        fprintf(stderr, "  [PRESENT] #%u: [0x759338]=0x%08X [0x7592B4]=0x%08X esi_param=0x%08X\n",
                g_present_count, rw_cb, im2d_fn, MEM32(esp + 4));
    }

    /* Call through to real gen code: reads 3 stack params, calls sub_001DE7E0 */
    eax = MEM32(esp + 0xC);
    ecx = MEM32(esp + 8);
    PUSH32(esp, esi);
    esi = MEM32(esp + 8);
    edx = MEM32(esi + 0x60);
    PUSH32(esp, eax);
    PUSH32(esp, ecx);
    PUSH32(esp, edx);
    PUSH32(esp, 0); sub_001DE7E0();

    /* loc_001D9438: cleanup */
    esp = esp + 0xC;
    eax = 0;
    POP32(esp, esi);

    /* ── Im2d debug HUD overlay ── */
    {
        /* Draw debug status bars via im2d:
         * - Top bar: game state indicator (color = state)
         * - Bottom bar: im2d pipeline active indicator */
        typedef struct { float x, y, z, rhw; uint32_t col; float u, v; } HudVert;
        HudVert hv[12]; /* 2 quads = 12 verts */
        int i;

        uint32_t game_st = MEM32(0x4D53B8);
        /* State color: 4=orange, 5=green, 7=blue, other=grey */
        uint32_t sc = 0xC0808080;
        if (game_st == 4) sc = 0xC0FF8000;
        if (game_st == 5) sc = 0xC000FF80;
        if (game_st == 7) sc = 0xC04080FF;

        /* Initialize all verts to defaults */
        for (i = 0; i < 12; i++) {
            hv[i].z = 0; hv[i].rhw = 1; hv[i].u = 0; hv[i].v = 0;
        }

        /* Top bar: game state indicator (full width, 4px tall) */
        hv[0].x=0;   hv[0].y=0; hv[0].col=sc;
        hv[1].x=640; hv[1].y=0; hv[1].col=sc;
        hv[2].x=0;   hv[2].y=4; hv[2].col=sc;
        hv[3].x=640; hv[3].y=0; hv[3].col=sc;
        hv[4].x=640; hv[4].y=4; hv[4].col=sc;
        hv[5].x=0;   hv[5].y=4; hv[5].col=sc;

        /* Bottom bar: im2d active indicator (green pulsing) */
        {
            uint32_t pc = 0xC000FF00 | ((g_present_count % 60 < 30) ? 0xFF000000 : 0x80000000);
            hv[6].x=0;    hv[6].y=476;  hv[6].col=pc;
            hv[7].x=640;  hv[7].y=476;  hv[7].col=pc;
            hv[8].x=0;    hv[8].y=480;  hv[8].col=pc;
            hv[9].x=640;  hv[9].y=476;  hv[9].col=pc;
            hv[10].x=640; hv[10].y=480; hv[10].col=pc;
            hv[11].x=0;   hv[11].y=480; hv[11].col=pc;
        }

        rw_bridge_im2d_render(4, hv, 12);
    }

    /* Always do our D3D11 Present */
    d3d8_PresentFrame();

    esp += 4; return;
}
void sub_001D9450(void) { esp += 4; return; }

/*
 * sub_001DDAF0 - RW display driver render callback
 *
 * This is the function pointer stored at [0x759338] = table[0x14].
 * Called by sub_001DE7E0 via indirect call with 3 params.
 * Original: drives NV2A GPU rendering for the current frame.
 *
 * For now, stub with tracing. This is where we'd hook D3D11 rendering
 * of the RW scene graph (menus, fonts, 2D elements).
 */
void sub_001DDAF0(void)
{
    /* Original code from XBE (80 bytes at 0x1DDAF0):
     *   1. Gamma state management using [0x41AB40]
     *   2. Call sub_00351090(0) - RW camera begin update / scene render
     *   3. Return 1
     *
     * The gamma calls go to sub_001D7130 (set_gamma) which we skip.
     * The key call is sub_00351090 which drives the scene render. */
    static uint32_t call_count = 0;
    uint32_t param3 = MEM32(esp + 0xC);  /* 3rd param, bit 0 = gamma flag */

    call_count++;
    if (call_count <= 5 || (call_count % 1000) == 0) {
        fprintf(stderr, "  [RW-RENDER] sub_001DDAF0 #%u: p3=0x%08X cam=[0x35FB48]=0x%08X\n",
                call_count, param3, MEM32(0x35FB48));
    }

    /* Reset bridge state for this new render pass */
    rw_bridge_new_frame();

    /* Skip gamma logic (sub_001D7130 touches NV2A gamma ramp).
     * Just set the state variable like the original does. */
    if (param3 & 1) {
        if (MEM32(0x41AB40) == 0x80000000u)
            MEM32(0x41AB40) = 1;
    } else {
        if (MEM32(0x41AB40) != 0x80000000u)
            MEM32(0x41AB40) = 0x80000000u;
    }

    /* Call sub_00351090(0) - RW camera scene render.
     * The bridge inside sub_00351090 will render through our D3D8→D3D11 layer. */
    PUSH32(esp, 0);
    PUSH32(esp, 0); sub_00351090();

    eax = 1;
    esp += 4; return;
}
void sub_001D94A0(void) { esp += 4; return; }
void sub_001D94D0(void) { esp += 4; return; }

/*
 * sub_001DD910 - RW CameraBeginUpdate (display driver entry 0x01)
 *
 * Original: Sets up rendering context for a new frame.
 * Called from RwCameraBeginUpdate with camera pointer as param.
 *
 * Override: Call bridge to set up BeginScene + viewport + transforms.
 * Also stores camera VA for later use by bridge.
 *
 * Stack: [esp+4]=ret_junk, [esp+8]=camera_ptr, [esp+C]=flags
 */
void sub_001DD910(void)
{
    static uint32_t call_count = 0;
    uint32_t camera_ptr = MEM32(esp + 8);

    call_count++;
    if (call_count <= 5 || (call_count % 500) == 0) {
        fprintf(stderr, "  [RW-BEGIN] CameraBeginUpdate #%u: camera=0x%08X\n",
                call_count, camera_ptr);
    }

    /* Store camera in RW global for sub_00351090 to read */
    MEM32(0x759280) = camera_ptr;

    /* Call bridge to set up BeginScene/Clear/viewport */
    rw_bridge_camera_begin(camera_ptr);

    eax = 1;
    esp += 4; return;
}

/*
 * sub_001E7B10 - RW CameraEndUpdate (display driver entry 0x0B)
 *
 * Original: Finalizes rendering for the current frame.
 * Called from RwCameraEndUpdate.
 *
 * Override: Call bridge to EndScene.
 */
void sub_001E7B10(void)
{
    static uint32_t call_count = 0;
    call_count++;
    if (call_count <= 5 || (call_count % 500) == 0) {
        fprintf(stderr, "  [RW-END] CameraEndUpdate #%u\n", call_count);
    }

    /* Call bridge to flush 2D and EndScene */
    rw_bridge_camera_end(0);

    eax = 1;
    esp += 4; return;
}

/*
 * sub_00351090 - RW camera scene render (RwCameraShowRaster equivalent)
 *
 * Original: 0x00351090 - 0x00351173 (227 bytes)
 * Source: RenderWare camera update / scene render orchestrator
 *
 * Gen code flow (from recomp_0022.c loc_003510BF..loc_003510F5):
 *   1. esi = [0x35FB48] (device context at 0x35D6A0)
 *   2. Pre-render hook (sub_00359740) if device+0x8D8 set
 *   3. If camera active (device+8 bit 14 = 0x4000):
 *      a. sub_00351490(0) - begin camera (NV2A state setup)
 *      b. Check device + (frame_counter-1 & 1)*4 + 0x1974 (RT surface)
 *      c. If non-NULL: sub_00351770_gen(1) - RENDER THE SCENE (62K)
 *      d. Increment frame counter at device+0x2478
 *   4. sub_00350C10(swap_flag) - end camera / show raster
 *   5. Bridge present for D3D11 output
 *
 * Convention: cdecl, 1 param on stack (swap_flag), ret 4
 */
extern ptrdiff_t g_xbox_mem_offset;
extern void sub_00351770_gen(void);

/* Camera tracking: cameras created by sub_001D9510 */
uint32_t g_created_cameras[8] = {0};
int g_created_camera_count = 0;

/* Gate for enabling the gen code render chain.
 * 0 = safe mode (bridge only, no gen calls)
 * 1 = call sub_00351490/sub_00351770_gen/sub_00350C10
 * Auto-enables after 60 frames of warmup. Toggle with G key. */
int g_gen_render_chain_enabled = 0;

void sub_00351090(void)
{
    uint32_t dev_va;
    uint32_t swap_flag;
    static uint32_t call_count = 0;

    call_count++;

    /* Gen render chain disabled — render dispatch now goes through manual
     * sub_001D7D10 bridge. The D3D8LTCG gen code (sub_00351490→sub_00351770_gen→
     * sub_00350C10) has unresolved mid-function stubs that cause hangs.
     * Enable manually with G key for debugging only. */

    /* Read device pointer from [0x35FB48], convert mirror addresses */
    dev_va = MEM32(0x35FB48);
    swap_flag = MEM32(esp + 4);

    if (dev_va > 0x4000000 && g_xbox_mem_offset > 0) {
        uint32_t _off32 = (uint32_t)g_xbox_mem_offset;
        if (dev_va >= _off32) {
            uint32_t maybe = (dev_va - _off32) % 0x04000000u;
            if (maybe >= 0x10000 && maybe < 0x4000000)
                dev_va = maybe;
        }
    }

    /* Default swap_flag to 5 if 0 (matches gen code loc_003510BA) */
    uint32_t ebx_flag = swap_flag ? swap_flag : 5;

    if (call_count <= 5 || (call_count % 500) == 0) {
        uint32_t flags = (dev_va > 0x10000 && dev_va < 0x4000000) ? MEM32(dev_va + 8) : 0;
        uint32_t frame = (dev_va > 0x10000 && dev_va < 0x4000000) ? MEM32(dev_va + 0x2478) : 0;
        uint32_t rt0 = (dev_va > 0x10000 && dev_va < 0x4000000) ? MEM32(dev_va + 0x1974) : 0;
        uint32_t rt1 = (dev_va > 0x10000 && dev_va < 0x4000000) ? MEM32(dev_va + 0x1978) : 0;
        fprintf(stderr, "  [RW-CAM] sub_00351090 #%u: dev=0x%08X flags=0x%X frame=%u "
                "swap=%u RT=0x%X/0x%X gen=%d\n",
                call_count, dev_va, flags, frame, ebx_flag, rt0, rt1,
                g_gen_render_chain_enabled);

        /* Dump device matrices (full 4x4) to understand format */
        if (dev_va > 0x10000 && dev_va < 0x4000000) {
            float *view = (float*)XBOX_PTR(dev_va + 0xCA0);
            float *proj = (float*)XBOX_PTR(dev_va + 0xC60);
            fprintf(stderr, "  [RW-CAM]   dev+0xCA0 (4x4): [%.4f %.4f %.4f %.4f]\n"
                            "                               [%.4f %.4f %.4f %.4f]\n"
                            "                               [%.4f %.4f %.4f %.4f]\n"
                            "                               [%.4f %.4f %.4f %.4f]\n",
                    view[0], view[1], view[2], view[3],
                    view[4], view[5], view[6], view[7],
                    view[8], view[9], view[10], view[11],
                    view[12], view[13], view[14], view[15]);
            fprintf(stderr, "  [RW-CAM]   dev+0xC60 (4x4): [%.4f %.4f %.4f %.4f]\n"
                            "                               [%.4f %.4f %.4f %.4f]\n"
                            "                               [%.4f %.4f %.4f %.4f]\n"
                            "                               [%.4f %.4f %.4f %.4f]\n",
                    proj[0], proj[1], proj[2], proj[3],
                    proj[4], proj[5], proj[6], proj[7],
                    proj[8], proj[9], proj[10], proj[11],
                    proj[12], proj[13], proj[14], proj[15]);

            /* Also check camera struct at 0x4D4008 — frame pointer and viewWindow */
            uint32_t cam_va = MEM32(0x4D5370);  /* active camera pointer */
            if (cam_va > 0x10000 && cam_va < 0x4000000) {
                uint32_t frame_ptr = MEM32(cam_va + 4);  /* RwObject.parent → RwFrame */
                float vw_x = MEMF(cam_va + 0x8C);
                float vw_y = MEMF(cam_va + 0x90);
                float near_clip = MEMF(cam_va + 0x80);
                float far_clip  = MEMF(cam_va + 0x84);
                fprintf(stderr, "  [RW-CAM]   cam=0x%08X frame=0x%08X vw=(%.3f,%.3f) near=%.3f far=%.1f\n",
                        cam_va, frame_ptr, vw_x, vw_y, near_clip, far_clip);
                /* Frame ptr may be Xbox VA or native (mirror) pointer */
                float *ltm = NULL;
                if (frame_ptr > 0x10000 && frame_ptr < 0x4000000) {
                    ltm = (float*)XBOX_PTR(frame_ptr + 0x58);
                } else if (frame_ptr >= (uint32_t)g_xbox_mem_offset &&
                           frame_ptr < (uint32_t)g_xbox_mem_offset + 0x4000000) {
                    /* Native pointer — use directly */
                    ltm = (float*)((uintptr_t)(frame_ptr + 0x58));
                } else if (frame_ptr > 0x10000000) {
                    /* Mirror view: modulo 64MB to get physical address, then use as Xbox VA */
                    uint32_t phys = (frame_ptr - (uint32_t)g_xbox_mem_offset) % 0x04000000u;
                    ltm = (float*)XBOX_PTR(phys + 0x58);
                }
                if (ltm) {
                    fprintf(stderr, "  [RW-CAM]   LTM: right=(%.2f,%.2f,%.2f) up=(%.2f,%.2f,%.2f)\n",
                            ltm[0], ltm[1], ltm[2], ltm[4], ltm[5], ltm[6]);
                    fprintf(stderr, "  [RW-CAM]   LTM: at=(%.2f,%.2f,%.2f) pos=(%.2f,%.2f,%.2f)\n",
                            ltm[8], ltm[9], ltm[10], ltm[12], ltm[13], ltm[14]);
                }
                /* Dump cameras created by sub_001D9510 (tracked dynamically) */
                for (int ci = 0; ci < g_created_camera_count && ci < 4; ci++) {
                    uint32_t cva = g_created_cameras[ci];
                    if (cva > 0x10000 && cva < 0x4000000) {
                        uint32_t cf = MEM32(cva + 4);
                        float cvw = MEMF(cva + 0x8C);
                        float cnr = MEMF(cva + 0x80);
                        float cfr = MEMF(cva + 0x84);
                        fprintf(stderr, "  [RW-CAM]   cam[%d]=0x%08X frame=0x%08X vw=%.3f near=%.3f far=%.1f\n",
                                ci, cva, cf, cvw, cnr, cfr);
                    }
                }
            }
        }
    }

    if (dev_va < 0x10000 || dev_va >= 0x4000000)
        goto done;

    /* Force camera active flag (bit 14 = 0x4000) at device+8.
     * The xemu device snapshot overwrites this after rw_state_init,
     * and the snapshot value (0xF81000 = PB base) doesn't have bit 14 set.
     * Gen code checks: if (TEST_Z(HI8(MEM32(esi+8)), 0x40)) skip render. */
    MEM32(dev_va + 8) = MEM32(dev_va + 8) | 0x4000;

    /* Also ensure render target surfaces stay non-NULL (other gen code may clear them) */
    if (MEM32(dev_va + 0x1974) == 0) MEM32(dev_va + 0x1974) = 0x3A1F;
    if (MEM32(dev_va + 0x1978) == 0) MEM32(dev_va + 0x1978) = 0x3A25;

    /* ═══ POPULATE CAMERA FRAME FROM PHYSICS (GAMEPLAY) ═══
     * When in gameplay and the camera frame LTM has no position, we compute
     * a chase camera from the physics body and write it to the frame.
     * This feeds into the device matrix update below and the bridge camera. */
    {
        uint32_t cam_va = MEM32(0x4D5370);
        /* Only populate when the gameplay camera is active — NOT during init
         * (state 4) or menus, to avoid corrupting the menu camera's frame */
        int in_gameplay = (cam_va == 0x4D45D0);

        if (in_gameplay && cam_va > 0x10000 && cam_va < 0x4000000) {
            uint32_t frame_raw = MEM32(cam_va + 4);

            /* If gameplay camera has no frame, allocate and attach one */
            if (frame_raw == 0) {
                static uint32_t gameplay_frame_va = 0;
                if (gameplay_frame_va == 0) {
                    gameplay_frame_va = xbox_HeapAlloc(0xA4, 1);
                    if (gameplay_frame_va) {
                        /* Initialize identity LTM */
                        float *f = (float*)XBOX_PTR(gameplay_frame_va + 0x58);
                        f[0]=1; f[1]=0; f[2]=0; f[3]=0;
                        f[4]=0; f[5]=1; f[6]=0; f[7]=0;
                        f[8]=0; f[9]=0; f[10]=1; f[11]=0;
                        f[12]=0; f[13]=0; f[14]=0; f[15]=0;
                        /* Also identity modelling matrix at +0x18 */
                        float *m = (float*)XBOX_PTR(gameplay_frame_va + 0x18);
                        m[0]=1; m[1]=0; m[2]=0; m[3]=0;
                        m[4]=0; m[5]=1; m[6]=0; m[7]=0;
                        m[8]=0; m[9]=0; m[10]=1; m[11]=0;
                        m[12]=0; m[13]=0; m[14]=0; m[15]=0;
                        MEM32(gameplay_frame_va + 0x98) = 0;  /* child */
                        MEM32(gameplay_frame_va + 0x9C) = 0;  /* next */
                        MEM32(gameplay_frame_va + 0xA0) = gameplay_frame_va;  /* root */
                        fprintf(stderr, "  [RW-CAM] Allocated frame 0x%08X for gameplay camera 0x%08X\n",
                                gameplay_frame_va, cam_va);
                    }
                }
                if (gameplay_frame_va) {
                    MEM32(cam_va + 4) = gameplay_frame_va;
                    frame_raw = gameplay_frame_va;
                }
            }

            float *ltm = NULL;
            if (frame_raw > 0x10000 && frame_raw < 0x4000000) {
                ltm = (float*)XBOX_PTR(frame_raw + 0x58);
            } else if (frame_raw > 0x10000000) {
                uint32_t phys = (frame_raw - (uint32_t)g_xbox_mem_offset) % 0x04000000u;
                if (phys > 0x10000 && phys < 0x4000000)
                    ltm = (float*)XBOX_PTR(phys + 0x58);
            }

            /* Populate camera frame LTM from physics body EVERY FRAME.
             * The chase camera must track the car continuously, not just on
             * the first frame. Previous code only ran when pos_mag2 < 1.0,
             * freezing the camera at the initial spawn position. */
            if (ltm) {
                /* Read physics body */
                uint32_t vel_ptr = MEM32(0x557880 + 0x1B4);
                float phys_x = 0, phys_z = 0, hdg = 0, spd = 0;
                if (vel_ptr > 0x100 && vel_ptr < 0x3FFFFFF) {
                    phys_x = MEMF(vel_ptr + 0x10);
                    phys_z = MEMF(vel_ptr + 0x14);
                    hdg = MEMF(vel_ptr + 0x18);
                    spd = MEMF(vel_ptr + 0x1C);
                } else {
                    phys_x = MEMF(0x5FFF10);
                    phys_z = MEMF(0x5FFF14);
                    hdg = MEMF(0x5FFF18);
                    spd = MEMF(0x5FFF1C);
                }

                if (phys_x != 0.0f || phys_z != 0.0f) {
                    /* Compute chase camera position */
                    float cam_dist = 15.0f + fabsf(spd) * 0.2f;
                    float sh = sinf(hdg), ch = cosf(hdg);
                    float cx = phys_x - sh * cam_dist;
                    float cy = 5.0f;
                    float cz = phys_z - ch * cam_dist;

                    /* Write position to frame LTM */
                    ltm[12] = cx;
                    ltm[13] = cy;
                    ltm[14] = cz;

                    /* Write chase camera orientation to frame LTM.
                     * Forward direction = heading vector toward car.
                     * RW uses right-handed: right=X, up=Y, at=Z (forward). */
                    float fx = sh, fz = ch;  /* forward = toward car (heading dir) */
                    /* right = cross(up, forward) = cross((0,1,0), (fx,0,fz)) = (fz,0,-fx) */
                    ltm[0] = fz;  ltm[1] = 0;     ltm[2] = -fx;  ltm[3] = 0;  /* right */
                    ltm[4] = 0;   ltm[5] = 1.0f;  ltm[6] = 0;    ltm[7] = 0;  /* up */
                    ltm[8] = fx;  ltm[9] = 0;     ltm[10] = fz;  ltm[11] = 0; /* at (forward) */
                    ltm[15] = 0;

                    static int logged_pop = 0;
                    if (!logged_pop || (call_count % 500) == 0) {
                        fprintf(stderr, "  [RW-CAM] Frame LTM from physics: "
                                "pos=(%.1f,%.1f,%.1f) hdg=%.1f° spd=%.1f\n",
                                cx, cy, cz, hdg * 57.2958f, spd);
                        logged_pop = 1;
                    }
                }
            }
        }
    }

    /* ═══ FORCE NEAR/FAR CLIP ON GAMEPLAY CAMERA ═══
     * Gen code (sub_00351490, sub_00351770_gen) resets the camera's near/far
     * clip planes each frame. Force valid values here so the projection
     * matrix can be built correctly below. */
    {
        uint32_t cam_va = MEM32(0x4D5370);
        if (cam_va == 0x4D45D0 && cam_va > 0x10000 && cam_va < 0x4000000) {
            float nc = MEMF(cam_va + 0x80);
            float fc = MEMF(cam_va + 0x84);
            if (fc <= nc || fc < 1.0f) {
                MEMF(cam_va + 0x80) = 0.1f;     /* near clip */
                MEMF(cam_va + 0x84) = 5000.0f;  /* far clip */
            }
            if (MEMF(cam_va + 0x8C) < 0.001f) {
                MEMF(cam_va + 0x8C) = 1.0f;     /* viewWindow.x */
                MEMF(cam_va + 0x90) = 0.75f;    /* viewWindow.y */
            }
        }
    }

    /* ═══ UPDATE DEVICE MATRICES FROM CAMERA FRAME ═══
     * Since RwCameraBeginUpdate is never called by the D3D8LTCG pipeline,
     * we populate the device view/projection matrices ourselves from the
     * active camera's RwFrame LTM. This ensures the gen code render chain
     * uses the correct camera transform.
     *
     * dev+0xCA0: Projection-like matrix (scale + depth)
     * dev+0xC60: View/flip matrix (coordinate system conversion)
     */
    {
        uint32_t cam_va = MEM32(0x4D5370);  /* Active camera */
        if (cam_va > 0x10000 && cam_va < 0x4000000) {
            /* Read viewWindow from camera struct */
            float vw_x = MEMF(cam_va + 0x8C);
            float vw_y = MEMF(cam_va + 0x90);
            float near_clip = MEMF(cam_va + 0x80);
            float far_clip  = MEMF(cam_va + 0x84);

            /* If camera has valid viewWindow, update the projection-like matrix at +0xCA0 */
            if (vw_x > 0.001f && vw_y > 0.001f && far_clip > near_clip) {
                float *proj = (float*)XBOX_PTR(dev_va + 0xCA0);
                /* RW-style projection: 1/vw_x and 1/vw_y are the scale factors */
                float sx = 1.0f / vw_x;
                float sy = 1.0f / vw_y;
                float q = far_clip / (far_clip - near_clip);

                proj[0]  = sx;   proj[1]  = 0;    proj[2]  = 0;    proj[3]  = 0;
                proj[4]  = 0;    proj[5]  = sy;   proj[6]  = 0;    proj[7]  = 0;
                proj[8]  = 0;    proj[9]  = 0;    proj[10] = q;    proj[11] = 1.0f;
                proj[12] = 0;    proj[13] = 0;    proj[14] = -q * near_clip; proj[15] = 0;
            }

            /* Read camera frame and update view matrix at +0xC60 */
            uint32_t frame_raw = MEM32(cam_va + 4);
            /* Resolve frame pointer (may be Xbox VA, native, or mirror) */
            float *ltm = NULL;
            if (frame_raw > 0x10000 && frame_raw < 0x4000000) {
                ltm = (float*)XBOX_PTR(frame_raw + 0x58);
            } else if (frame_raw > 0x10000000) {
                uint32_t phys = (frame_raw - (uint32_t)g_xbox_mem_offset) % 0x04000000u;
                if (phys > 0x10000 && phys < 0x4000000)
                    ltm = (float*)XBOX_PTR(phys + 0x58);
            }

            if (ltm) {
                float px = ltm[12], py = ltm[13], pz = ltm[14];
                float pos_mag2 = px*px + py*py + pz*pz;
                /* Only update device matrices when camera has a real position
                 * (at least 1 unit from origin). Menu cameras sit at (0,0,0)
                 * and their LTM orientation may be garbage. */
                if (pos_mag2 > 1.0f) {
                    /* Build view matrix from frame LTM (inverse of world transform) */
                    float *view = (float*)XBOX_PTR(dev_va + 0xC60);
                    float rx = ltm[0], ry = ltm[1], rz = ltm[2];   /* right */
                    float ux = ltm[4], uy = ltm[5], uz = ltm[6];   /* up */
                    float ax = ltm[8], ay = ltm[9], az = ltm[10];  /* at */

                    /* Transpose rotation, negate-dot translation */
                    view[0]  = rx;   view[1]  = ux;   view[2]  = ax;   view[3]  = 0;
                    view[4]  = ry;   view[5]  = uy;   view[6]  = ay;   view[7]  = 0;
                    view[8]  = rz;   view[9]  = uz;   view[10] = az;   view[11] = 0;
                    view[12] = -(rx*px + ry*py + rz*pz);
                    view[13] = -(ux*px + uy*py + uz*pz);
                    view[14] = -(ax*px + ay*py + az*pz);
                    view[15] = 1.0f;

                    static int logged_update = 0;
                    if (!logged_update || (call_count % 500) == 0) {
                        fprintf(stderr, "  [RW-CAM] Updated device matrices from camera 0x%08X frame LTM\n"
                                "           pos=(%.1f,%.1f,%.1f) vw=(%.3f,%.3f)\n",
                                cam_va, px, py, pz, vw_x, vw_y);
                        logged_update = 1;
                    }
                }
            }
        }
    }

    if (g_gen_render_chain_enabled && (ebx_flag & 3)) {
        /* ═══ GEN CODE RENDER CHAIN ═══
         * Calls the real D3D8LTCG pipeline. Requires fully initialized device
         * context — sub_00351490 walks deep pointer chains in the device state.
         * Currently gated behind g_gen_render_chain_enabled (G key toggle). */

        /* Reset PB write position to start of ring before each frame.
         * The gen code writes NV2A push buffer commands; we parse them after.
         * Without reset, writes accumulate and eventually overflow the 4MB ring. */
        {
            /* Use known good PB addresses (device+0x28 gets corrupted by gen code).
             * PB base is at MEM32(0x35D69C), set during init and not touched by gen code.
             * Ring size is at device+0x44 (4MB). */
            uint32_t pb_base = MEM32(0x35D69C);
            uint32_t pb_size = MEM32(dev_va + 0x44);
            uint32_t pb_end  = pb_base + pb_size;
            /* Re-apply all PB ring fields every frame (gen code clobbers them) */
            MEM32(dev_va + 0x00) = pb_base;    /* device current write cursor */
            MEM32(dev_va + 0x04) = pb_end;     /* device segment limit */
            MEM32(dev_va + 0x08) = pb_base;    /* device secondary write cursor */
            MEM32(dev_va + 0x24) = pb_base;    /* PB ring base */
            MEM32(dev_va + 0x28) = pb_end;     /* PB ring end */
            MEM32(dev_va + 0x2C) = pb_base;    /* PB write sequence position */
            MEM32(0x35D6A0) = pb_base;          /* global PB write pointer */
            /* Set fake GPU read = 0xFFFFFFFF so ALL fence waits exit.
             * Fence pattern: exit when gpu_read >= fence_marker (edi).
             * 0xFFFFFFFF is always >= any uint32 value → immediate exit.
             * Space check is patched out in sub_00351770_gen. */
            MEM32(dev_va + 0x3004) = 0xFFFFFFFFu;
            MEM32(dev_va + 0x30) = dev_va + 0x3004;
        }

        /* Step A: sub_00351490(0) — begin camera (NV2A state setup) */
        PUSH32(esp, 0);
        PUSH32(esp, 0); sub_00351490();

        /* Step B: Check render target surface (double-buffered) */
        {
            uint32_t frame_counter = MEM32(dev_va + 0x2478);
            uint32_t rt_idx = (frame_counter - 1) & 1;
            uint32_t rt_surface = MEM32(dev_va + rt_idx * 4 + 0x1974);

            if (rt_surface != 0) {
                /* Step C: sub_00351770_gen(1) — SCENE RENDER (62K) */
                eax = 1;
                PUSH32(esp, 1);
                PUSH32(esp, 0); sub_00351770_gen();

                {
                    uint32_t pb_base = MEM32(dev_va + 0x24);
                    uint32_t pb_after = MEM32(dev_va + 0x00);
                    uint32_t pb_written = (pb_after > pb_base) ? pb_after - pb_base : 0;
                    if (call_count <= 5 || (call_count % 500) == 0 || (pb_written > 100 && call_count < 200))
                        fprintf(stderr, "  [RW-CAM] sub_00351770_gen: eax=0x%X pb_written=%u bytes (%u dwords)\n",
                                eax, pb_written, pb_written / 4);
                }
            }
        }

        /* Step D: Increment frame counter */
        MEM32(dev_va + 0x2478) = MEM32(dev_va + 0x2478) + 1;

        /* Step E: sub_00350C10(swap_flag) — end camera */
        PUSH32(esp, ebx_flag);
        PUSH32(esp, 0); sub_00350C10();
    } else {
        /* ═══ SAFE MODE (default) ═══
         * Just maintain frame counter. Bridge handles D3D11 rendering. */
        MEM32(dev_va + 0x2478) = MEM32(dev_va + 0x2478) + 1;
    }

    /* Bridge render: handles D3D11 present (menu PB replay, gameplay, etc.)
     * Track geometry injection now happens inside the bridge's gameplay path
     * (within the BeginScene/EndScene bracket) via D3D8 DrawPrimitiveUP. */
    rw_bridge_camera_render(dev_va);

done:
    eax = (dev_va > 0x10000 && dev_va < 0x4000000)
          ? MEM32(dev_va + 0x2478) : 0;
    esp += 4; /* pop return address */
    esp += 4; /* clean 1 param (ret 4) */
    return;
}

/*
 * sub_001D9510 - RW camera create (rw_core, src/bacamera.c)
 *
 * Original: 0x001D9510 - 0x001D9640 (304 bytes, 76 insns)
 *
 * Creates an RW camera object. The original calls ICALL(MEM32(0x7593E4))
 * to allocate a GPU raster, then initializes camera struct fields:
 *   +0x00: type (4 = camera)
 *   +0x10/0x18/0x1C: vtable entries (camera methods)
 *   +0x14: flags (1)
 *   +0x60/0x64: parent pointers (null)
 *   +0x68-0x74: clip planes (from 0x3B168C constant)
 *   +0x78-0x7C: zero
 *   +0x80: near clip (from 0x36C2D4)
 *   +0x84: far clip (from 0x36C418)
 *   +0x88: fog distance (from 0x3B1694)
 *   +0x8C-0x90: computed view window params
 *   +0x2C: zero
 *
 * Our override: allocate camera + frame structs in Xbox heap, set all fields
 * matching the gen code, skip the raster alloc (not needed for D3D11 path).
 *
/*
 * Fixed issues vs. previous version:
 *   1. Allocate an RwFrame and set camera+0x04 = frame_va (was NULL)
 *   2. Initialize frame LTM to identity (was garbage)
 *   3. Set sensible viewWindow defaults when viewport globals are 0
 *   4. Override near/far to racing-appropriate values
 */
void sub_001D9510(void)
{
    /* Allocate camera struct in Xbox heap (0x220 bytes to cover full RwCamera + plugins) */
    uint32_t cam_va = xbox_HeapAlloc(0x220, 1);
    if (cam_va == 0) {
        fprintf(stderr, "  [RW-CAM] sub_001D9510: heap alloc failed (camera)\n");
        eax = 0;
        esp += 4; return;
    }

    /* Allocate RwFrame (0xA4 = 164 bytes, zero-initialized) */
    uint32_t frame_va = xbox_HeapAlloc(0xA4, 1);
    if (frame_va == 0) {
        fprintf(stderr, "  [RW-CAM] sub_001D9510: heap alloc failed (frame)\n");
        eax = 0;
        esp += 4; return;
    }

    /* ── Initialize RwFrame ── */
    /* +0x00: RwObject (type=0=frame, parent=0) */
    MEM8(frame_va) = 0;         /* type = rwFRAME */
    MEM32(frame_va + 4) = 0;    /* parent = NULL */

    /* +0x18: modelling matrix (identity) */
    MEMF(frame_va + 0x18) = 1.0f;  /* right.x */
    MEMF(frame_va + 0x1C) = 0.0f;
    MEMF(frame_va + 0x20) = 0.0f;
    MEMF(frame_va + 0x24) = 0.0f;  /* flags/pad */
    MEMF(frame_va + 0x28) = 0.0f;
    MEMF(frame_va + 0x2C) = 1.0f;  /* up.y */
    MEMF(frame_va + 0x30) = 0.0f;
    MEMF(frame_va + 0x34) = 0.0f;
    MEMF(frame_va + 0x38) = 0.0f;
    MEMF(frame_va + 0x3C) = 0.0f;
    MEMF(frame_va + 0x40) = 1.0f;  /* at.z */
    MEMF(frame_va + 0x44) = 0.0f;
    MEMF(frame_va + 0x48) = 0.0f;  /* pos.x */
    MEMF(frame_va + 0x4C) = 0.0f;  /* pos.y */
    MEMF(frame_va + 0x50) = 0.0f;  /* pos.z */
    MEMF(frame_va + 0x54) = 0.0f;  /* pad3 / matrix flags */

    /* +0x58: LTM (local-to-world, identity) */
    MEMF(frame_va + 0x58) = 1.0f;  /* right.x */
    MEMF(frame_va + 0x5C) = 0.0f;
    MEMF(frame_va + 0x60) = 0.0f;
    MEMF(frame_va + 0x64) = 0.0f;
    MEMF(frame_va + 0x68) = 0.0f;
    MEMF(frame_va + 0x6C) = 1.0f;  /* up.y */
    MEMF(frame_va + 0x70) = 0.0f;
    MEMF(frame_va + 0x74) = 0.0f;
    MEMF(frame_va + 0x78) = 0.0f;
    MEMF(frame_va + 0x7C) = 0.0f;
    MEMF(frame_va + 0x80) = 1.0f;  /* at.z */
    MEMF(frame_va + 0x84) = 0.0f;
    MEMF(frame_va + 0x88) = 0.0f;  /* pos.x */
    MEMF(frame_va + 0x8C) = 0.0f;  /* pos.y */
    MEMF(frame_va + 0x90) = 0.0f;  /* pos.z */
    MEMF(frame_va + 0x94) = 0.0f;

    /* +0x98: child/next/root */
    MEM32(frame_va + 0x98) = 0;          /* child = NULL */
    MEM32(frame_va + 0x9C) = 0;          /* next = NULL */
    MEM32(frame_va + 0xA0) = frame_va;   /* root = self */

    /* ── Initialize RwCamera ── */
    MEM8(cam_va) = 4;        /* type = rwCAMERA */
    MEM8(cam_va + 1) = 0;
    MEM8(cam_va + 2) = 0;
    MEM8(cam_va + 3) = 0;
    MEM32(cam_va + 4) = frame_va;  /* parent → RwFrame (was NULL!) */

    /* +0x08: inFrame link (point to self for valid linked list) */
    MEM32(cam_va + 0x08) = cam_va + 0x08;  /* next → self */
    MEM32(cam_va + 0x0C) = cam_va + 0x08;  /* prev → self */

    /* RW camera vtable entries (Xbox VAs of recompiled functions) */
    MEM32(cam_va + 0x10) = 0x1D9130;   /* camera sync callback */
    MEM32(cam_va + 0x14) = 1;          /* flags = active */
    MEM32(cam_va + 0x18) = 0x1D91B0;   /* end update callback */
    MEM32(cam_va + 0x1C) = 0x1D9180;   /* clear callback */

    /* Clip planes (from .rdata constants) */
    float clip_val = MEMF(0x3B168C);
    MEMF(cam_va + 0x68) = clip_val;
    MEMF(cam_va + 0x6C) = clip_val;
    MEMF(cam_va + 0x70) = clip_val;
    MEMF(cam_va + 0x74) = clip_val;
    MEMF(cam_va + 0x78) = 0.0f;
    MEMF(cam_va + 0x7C) = 0.0f;

    /* Near/far clip — use .rdata values but override if too small for racing */
    float near_clip = MEMF(0x36C2D4);
    float far_clip  = MEMF(0x36C418);
    if (far_clip < 100.0f) far_clip = 5000.0f;  /* Racing needs large far plane */
    if (near_clip < 0.01f) near_clip = 0.1f;
    MEMF(cam_va + 0x80) = near_clip;
    MEMF(cam_va + 0x84) = far_clip;
    MEMF(cam_va + 0x88) = MEMF(0x3B1694);  /* fog distance */

    /* viewWindow — compute from viewport globals, with sensible fallback */
    {
        float vw_min = MEMF(0x7592B8);
        float vw_max = MEMF(0x7592BC);
        float vw_x, vw_y;
        if (vw_max - vw_min > 0.001f) {
            /* Original gen code computation */
            float half_range = (vw_max - vw_min) * MEMF(0x3B188C);
            float vw_lo = half_range + vw_min;
            float vw_hi = vw_max - half_range;
            vw_x = (vw_hi - vw_lo) * MEMF(0x36C414);
            vw_y = ((vw_hi + vw_lo) - vw_x * MEMF(0x36C410)) * MEMF(0x3B1684);
        } else {
            /* Fallback: standard RW camera for 640x480 (4:3 aspect, ~90° HFOV) */
            vw_x = 1.0f;
            vw_y = 0.75f;
        }
        MEMF(cam_va + 0x8C) = vw_x;
        MEMF(cam_va + 0x90) = vw_y;
    }

    /* +0x40: projection type (1 = perspective) */
    MEM32(cam_va + 0x40) = 1;

    /* Other fields */
    MEM32(cam_va + 0x60) = 0;    /* clump ref */
    MEM32(cam_va + 0x64) = 0;
    MEM32(cam_va + 0x2C) = 0;

    /* Also link frame's objectList to camera */
    MEM32(frame_va + 0x10) = cam_va + 0x08;  /* objectList.next → camera's inFrame */
    MEM32(frame_va + 0x14) = cam_va + 0x08;  /* objectList.prev → camera's inFrame */

    static int cam_count = 0;
    if (g_created_camera_count < 8)
        g_created_cameras[g_created_camera_count++] = cam_va;
    cam_count++;
    if (cam_count <= 5)
        fprintf(stderr, "  [RW-CAM] sub_001D9510: camera=0x%08X frame=0x%08X "
                "vw=(%.3f,%.3f) near=%.2f far=%.1f\n",
                cam_va, frame_va,
                MEMF(cam_va + 0x8C), MEMF(cam_va + 0x90),
                MEMF(cam_va + 0x80), MEMF(cam_va + 0x84));

    eax = cam_va;
    esp += 4; return;
}

/**
/**
 * sub_001D7D10 - RW render dispatch → D3D8 bridge (OVERRIDE)
 *
 * Original: 0x001D7D10 - 0x001D7D2A (26 bytes, 9 insns)
 * CC: cdecl, 3 params (geometry params from render entry +0x80/+0x84/+0x88)
 * Caller: sub_001AD350 (render list dispatch)
 * Caller cleanup: esp += 0xC after call
 *
 * Original flow: sub_001D7040 (RW driver setup) → sub_00350000 (D3D8LTCG render)
 * Override: bypasses D3D8LTCG and counts draws for diagnostics.
 * The render entry data contains vertex/index pointers from static.dat
 * that the D3D8LTCG code would use to write NV2A push buffer commands.
 */
uint32_t g_current_geom_base = 0;  /* set by sub_001AD350 for sub_001D7D10 */
static uint32_t g_1D7D10_draws = 0;
void sub_001D7D10(void)
{
    /* Params from render entry geometry object at edi+0x80/+0x84/+0x88:
     *   param1 = NV2A draw mode (5=tristrip, 6=trilist)
     *   param2 = index count
     *   param3 = index buffer offset from geometry base
     *
     * The geometry object layout (0x90 bytes per draw):
     *   +0x00..+0x7F: bounding box (8 × float3)
     *   +0x80: draw mode
     *   +0x84: index count
     *   +0x88: index buffer offset (relative to geom base from entry+0x4C)
     *   +0x8C: end marker (0xFFFFFFFF)
     *
     * Vertex buffer starts at geometry_base + 0x90 (offset TBD)
     * 28-byte stride: float3 pos, uint32 packed_normal, uint32 color, float2 UV
     *
     * We reconstruct the geometry object address from the caller's edi register
     * which was set to entry_base + running_offset before pushing params. */
    uint32_t draw_mode = MEM32(esp + 4);
    uint32_t idx_count = MEM32(esp + 8);
    uint32_t idx_offset = MEM32(esp + 12);

    g_1D7D10_draws++;

    /* The caller (sub_001AD350) set edi = geom_base + running_offset.
     * It then read params from edi+0x80/84/88. So the geom object base
     * was at the address that stored these params. We can recover it
     * from the stack — the caller pushed edi's derived values.
     * Actually: edi was stored on the stack at esp+0x28 in the caller.
     * But we can compute: geom_base is in the entry at ebx+0x4C.
     *
     * Simpler approach: idx_offset is relative to geom_base (entry+0x4C).
     * The render entry's geom_base was used by the caller. Since we can't
     * easily access edi from here, let's look at the pattern:
     * The 3rd param (idx_offset) = 0x0A14 for draw #1.
     * Geom_base = 0x022CC060. So idx_buf = 0x022CC060 + 0x0A14 = 0x022CCA74.
     * But the caller passed idx_offset as the RAW value from edi+0x88 which
     * is ALREADY an absolute pointer (after relocation by sub_0019B4E0).
     * So param3 IS the absolute index buffer pointer, not an offset! */
    if (draw_mode >= 1 && draw_mode <= 8 &&
        idx_count > 0 && idx_count < 100000 &&
        idx_offset > 0x10000 && idx_offset < 0x4000000)
    {
        IDirect3DDevice8 *dev = xbox_GetD3DDevice();
        if (dev) {
            /* Index buffer: uint16 array at idx_offset */
            uint16_t *indices = (uint16_t*)XBOX_PTR(idx_offset);

            /* Vertex buffer: we need to find it. The geometry object at
             * (idx_offset - entry_idx_offset) has vertices starting at +0x90.
             * But we don't have the geom base here directly.
             *
             * Alternative: scan backwards from the index buffer to find the
             * bounding box / vertex data start. The vertex data starts at
             * geom_base + some_header_size. Since geom_base+0x80 = draw_mode,
             * and geom_base+0x88 = idx_offset, we can compute:
             * geom_base = (address where +0x88 == idx_offset).
             * But that's circular. Let's use a different approach.
             *
             * The geometry data is contiguous: [bbox 0x80][params 0x10][vertices...][indices]
             * So vertex_start = geom_base + 0x90, and the indices reference
             * vertices starting from index 0 at vertex_start.
             * We need geom_base. Since this function is called per-draw from
             * sub_001AD350 which tracks the geometry base, let's use a global. */
            extern uint32_t g_current_geom_base;
            if (g_current_geom_base > 0x10000 && g_current_geom_base < 0x4000000) {
                /* Source vertex buffer at geom_base + 0x90.
                 * Format: 28 bytes (float3 pos, uint32 packed_normal, uint32 color, float2 UV)
                 * Need to convert to 24-byte FVF (float3 pos, uint32 color, float2 UV)
                 * by skipping the packed_normal dword. */
                uint8_t *src_vb = (uint8_t*)XBOX_PTR(g_current_geom_base + 0x90);

                /* Find max vertex index to know how many verts to convert */
                uint32_t max_idx = 0;
                for (uint32_t i = 0; i < idx_count; i++)
                    if (indices[i] > max_idx) max_idx = indices[i];
                uint32_t vert_count = max_idx + 1;

                uint32_t prim_count = 0;
                uint32_t d3d_type = 4; /* D3DPT_TRIANGLELIST */
                if (draw_mode == 5) { prim_count = (idx_count > 2) ? idx_count - 2 : 0; d3d_type = 5; }
                else if (draw_mode == 6) { prim_count = idx_count / 3; d3d_type = 4; }

                if (prim_count > 0 && vert_count < 65536) {
                    /* Convert: skip packed_normal, brighten vertex colors.
                     * Xbox vertex colors are pre-lit ambient (~20-30 range).
                     * Boost them so geometry is visible without full lighting. */
                    static uint8_t cvt_buf[65536 * 24];
                    for (uint32_t v = 0; v < vert_count; v++) {
                        uint8_t *s = src_vb + v * 28;
                        uint8_t *d = cvt_buf + v * 24;
                        memcpy(d, s, 12);       /* pos (float3) */
                        /* Color: Xbox D3DCOLOR is ARGB uint32, same as PC.
                         * Brighten by shifting up (ambient values are 20-80). */
                        uint32_t col = *(uint32_t*)(s + 16);
                        uint32_t r = (col >> 16) & 0xFF;
                        uint32_t g = (col >> 8) & 0xFF;
                        uint32_t b = col & 0xFF;
                        r = (r * 3 > 255) ? 255 : r * 3;
                        g = (g * 3 > 255) ? 255 : g * 3;
                        b = (b * 3 > 255) ? 255 : b * 3;
                        *(uint32_t*)(d + 12) = 0xFF000000 | (r << 16) | (g << 8) | b;
                        memcpy(d+16, s+20, 8);  /* UV (float2) */
                    }

                    /* Set vertex format and render state */
                    dev->lpVtbl->SetVertexShader(dev,
                        D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1);
                    dev->lpVtbl->SetRenderState(dev, D3DRS_LIGHTING, FALSE);

                    /* Set view/projection for world-space track geometry.
                     * Build a look-at view + perspective projection targeting
                     * the track center (from loaded track data). */
                    {
                        extern float g_track_spawn_x, g_track_spawn_z;
                        extern TrackData g_track_data;
                        extern int g_track_loaded;
                        float cx = g_track_loaded ? g_track_data.center[0] : 1970.0f;
                        float cy = g_track_loaded ? g_track_data.center[1] + 200.0f : 200.0f;
                        float cz = g_track_loaded ? g_track_data.center[2] : 1420.0f;
                        float tx = cx, ty = cy - 200.0f, tz = cz + 100.0f;

                        /* Simple look-at view matrix */
                        float fx = tx-cx, fy = ty-cy, fz = tz-cz;
                        float fl = sqrtf(fx*fx+fy*fy+fz*fz);
                        if (fl > 0.001f) { fx/=fl; fy/=fl; fz/=fl; }
                        float rx = fy*0-0*fz, ry = fz*0-fx*0, rz = fx*0-fy*0;
                        /* cross(f, up(0,1,0)) */
                        rx = fy*0.0f - fz*1.0f; /* not right, let me simplify */

                        D3DMATRIX vm;
                        memset(&vm, 0, sizeof(vm));
                        /* Look down at track center from above */
                        vm._11 = 1; vm._22 = 0; vm._23 = -1; vm._32 = 1; vm._33 = 0;
                        vm._41 = -cx; vm._42 = -cz; vm._43 = cy;
                        vm._44 = 1.0f;

                        /* Perspective projection */
                        float fov = 1.0f; /* ~60 degrees */
                        float aspect = 640.0f / 480.0f;
                        float znear = 1.0f, zfar = 5000.0f;
                        D3DMATRIX pm;
                        memset(&pm, 0, sizeof(pm));
                        pm._11 = fov / aspect;
                        pm._22 = fov;
                        pm._33 = zfar / (zfar - znear);
                        pm._34 = 1.0f;
                        pm._43 = -znear * zfar / (zfar - znear);

                        D3DMATRIX wm;
                        memset(&wm, 0, sizeof(wm));
                        wm._11 = wm._22 = wm._33 = wm._44 = 1.0f;

                        dev->lpVtbl->SetTransform(dev, D3DTS_VIEW, &vm);
                        dev->lpVtbl->SetTransform(dev, D3DTS_PROJECTION, &pm);
                        dev->lpVtbl->SetTransform(dev, D3DTS_WORLD, &wm);
                    }
                    dev->lpVtbl->SetRenderState(dev, D3DRS_ZENABLE, TRUE);
                    dev->lpVtbl->SetRenderState(dev, D3DRS_ZWRITEENABLE, TRUE);
                    dev->lpVtbl->SetRenderState(dev, D3DRS_CULLMODE, D3DCULL_NONE);

                    /* Bind texture from static texture dictionary.
                     * Use the render state index from sub_0034EDB0's last call.
                     * sub_0034EDB0 stores the index at 0x35D2C0 table. Read the
                     * most recently set render state's associated texture.
                     * For now: use geom_base to derive a texture index — each
                     * geometry group in static.dat corresponds to a texture. */
                    {
                        extern StaticTexDict *rw_get_static_textures(void);
                        StaticTexDict *stex = rw_get_static_textures();
                        if (stex && stex->count > 0) {
                            uint32_t tex_idx = (g_current_geom_base >> 8) % (uint32_t)stex->count;
                            IDirect3DTexture8 *tex = stex->entries[tex_idx].texture;
                            if (tex) {
                                dev->lpVtbl->SetTexture(dev, 0, (IDirect3DBaseTexture8*)tex);
                                /* Configure texture stage to modulate texture × vertex color */
                                dev->lpVtbl->SetTextureStageState(dev, 0,
                                    D3DTSS_COLOROP, D3DTOP_MODULATE);
                                dev->lpVtbl->SetTextureStageState(dev, 0,
                                    D3DTSS_COLORARG1, D3DTA_TEXTURE);
                                dev->lpVtbl->SetTextureStageState(dev, 0,
                                    D3DTSS_COLORARG2, D3DTA_DIFFUSE);
                                dev->lpVtbl->SetTextureStageState(dev, 0,
                                    D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
                                dev->lpVtbl->SetTextureStageState(dev, 0,
                                    D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
                            }
                        }
                    }

                    dev->lpVtbl->DrawIndexedPrimitiveUP(dev,
                        d3d_type, 0, vert_count, prim_count,
                        indices, D3DFMT_INDEX16,
                        cvt_buf, 24);
                }
            }
        }
    }

    /* removed auto-screenshot — use F12 */

    if (g_1D7D10_draws <= 5 || (g_1D7D10_draws % 50000) == 0) {
        extern uint32_t g_current_geom_base;
        fprintf(stderr, "  [D3D-DRAW] #%u: mode=%u idxs=%u idx_ptr=0x%08X geom=0x%08X\n",
                g_1D7D10_draws, draw_mode, idx_count, idx_offset, g_current_geom_base);
    }

    esp += 4; return; /* ret — caller cleans 3 params with esp += 0xC */
}

/**
 * sub_001D7D50 - Xbox render state setter entry point (STUB)
 * sub_001D7D70 - Xbox render state setter #2 (STUB)
 *
 * Original: 0x001D7D50-0x001D7D6F (31 bytes), 0x001D7D70-0x001D7D85 (21 bytes)
 * Category: rw_driver_xbox, cdecl
 *
 * Called from RW core (sub_001C69C0) to set Xbox GPU render states via
 * sub_001D7040 and sub_0034FD80. These chain into sub_001D9A50/sub_001D9AF0
 * which iterate over GPU-allocated memory (0x92-0x93 Xbox VA range) producing
 * millions of VEH fault-skips. Stubbed since rendering uses D3D11.
 */
static uint32_t g_1D7D50_count = 0;
void sub_001D7D50(void) {
    g_1D7D50_count++;
    if (g_1D7D50_count <= 3 || (g_1D7D50_count % 10000) == 0)
        fprintf(stderr, "  [TRACE] sub_001D7D50 called #%u\n", g_1D7D50_count);
    esp += 4; return;
}
void sub_001D7D70(void) { esp += 4; return; }

/**
 * sub_001D9700 - Xbox render state dispatch (STUB)
 *
 * Original: 0x001D9700-0x001D987A (378 bytes, 106 insns)
 * Category: rw_driver_xbox, cdecl, 1 param (render state ID 1-30)
 *
 * Dispatch function: takes a render state ID, decrements, and does indirect
 * tail jump through a 30-entry table at 0x1D98F8 to handlers like
 * sub_001D9A50, sub_001D9AF0, sub_001D9BC0. Returns 1 for valid IDs.
 * Stubbed: return 1 (success) without setting any GPU state.
 */
void sub_001D9700(void) { eax = 1; esp += 4; return; }

/**
 * sub_001D9A50 - Xbox render state handler (STUB)
 * sub_001D9AF0 - Xbox render state handler #2 (STUB)
 * sub_001D9BC0 - Xbox render state handler #3 (STUB)
 * sub_001D9D40 - Xbox render state handler #4 (STUB)
 *
 * Category: rw_driver_xbox, cdecl
 *
 * These are dispatch targets of sub_001D9700. They read/write GPU render
 * state tables at 0x75D2A0-0x75D9B0 using eax as an index. When eax
 * contains garbage (e.g. 0x3F800001 = float 1.0), the computed addresses
 * land in the 0x92-0x93 Xbox VA range (GPU-allocated memory), producing
 * 10 million+ VEH fault-skips per call. Stubbed as no-ops.
 */
void sub_001D9A50(void) { eax = 1; esp += 4; return; }
void sub_001D9AF0(void) { eax = 1; esp += 4; return; }
void sub_001D9BC0(void) { eax = 1; esp += 4; return; }
void sub_001D9D40(void) { eax = 1; esp += 4; return; }

/**
 * sub_001C1740 - RW scene graph child list merge/traversal (STUB)
 *
 * Original: 0x001C1740-0x001C1796 (86 bytes, 35 insns)
 * CC: thiscall_cdecl (ecx = this), 0 stack params
 *
 * Recursive tree traversal: reads child list from this+0x44, recursively
 * processes each child (ecx = child + 8), then merges child list into parent.
 * With garbage scene graph (no real D3D objects), this chases random
 * pointers and overflows the native stack (1M+ nested calls).
 * Stubbed as no-op since scene graph data isn't initialized.
 */
void sub_001C1740(void) { esp += 4; return; }

/* ═══════════════════════════════════════════════════════════════════════
 * Mid-function entry point stubs (exposed by switch table fix)
 *
 * These addresses are mid-function entry points that the recompiler
 * didn't recognize as separate functions. They're now called from
 * code paths made reachable by the switch table fix. Stubbed as no-ops
 * until proper implementations can be created.
 * ═══════════════════════════════════════════════════════════════════════ */

/* 0x00014FB0: gap after sub_00014D80 (end 0x14F99), +0x17 past end */
void sub_00014FB0(void) { esp += 4; return; }

/* 0x0006AE80: gap after sub_0006ADE0 (end 0x6AE70), +0x10 past end */
void sub_0006AE80(void) { esp += 4; return; }

/* 0x000983E0: gap after sub_00098370 (end 0x983C5), +0x1B past end */
void sub_000983E0(void) { esp += 4; return; }

/* 0x000E32F0: gap after sub_000E3140 (end 0xE32DF), +0x11 past end */
void sub_000E32F0(void) { esp += 4; return; }

/* 0x00154270: inside sub_0015414B (+0x125) */
void sub_00154270(void) { esp += 4; return; }

/* 0x0015BC50: inside sub_0015B7AB (+0x4A5) */
void sub_0015BC50(void) { esp += 4; return; }

/* 0x00169BD0: inside sub_00169540 (+0x690) */
void sub_00169BD0(void) { esp += 4; return; }

/* 0x00188BA9: at end of sub_001888F0 */
void sub_00188BA9(void) { esp += 4; return; }

/* 0x0018BF50: inside sub_0018BED0 (+0x80) */
void sub_0018BF50(void) { esp += 4; return; }

/* 0x001BDD50: inside sub_001BDCD0 (+0x80) */
void sub_001BDD50(void) { esp += 4; return; }

/* 0x002F55AC: inside sub_002F5594 (+0x18) - RenderWare */
void sub_002F55AC(void) { esp += 4; return; }

/* 0x002F6770: inside sub_002F6750 (+0x20) - RenderWare */
void sub_002F6770(void) { esp += 4; return; }

/* 0x0031AAE7: inside sub_0031AADC (+0xB) - RenderWare D3D */
void sub_0031AAE7(void) { esp += 4; return; }

/* 0x0031AB7A: inside sub_0031AB59 (+0x21) - RenderWare D3D */
void sub_0031AB7A(void) { esp += 4; return; }

/* 0x0031AB90: inside sub_0031AB85 (+0xB) - RenderWare D3D */
void sub_0031AB90(void) { esp += 4; return; }

/* 0x0031ABB1: inside sub_0031AB85 (+0x2C) - RenderWare D3D */
void sub_0031ABB1(void) { esp += 4; return; }

/* 0x0031ABDD: inside sub_0031AB85 (+0x58) - RenderWare D3D */
void sub_0031ABDD(void) { esp += 4; return; }

/* 0x0031ABE8: inside sub_0031AB85 (+0x63) - RenderWare D3D */
void sub_0031ABE8(void) { esp += 4; return; }

/* 0x0034FBA0: inside sub_0034FAF0 (+0xB0) - RenderWare */
void sub_0034FBA0(void) { esp += 4; return; }

/* Round 2: more mid-function entry points (block-splitting fix) */
void sub_0006E680(void) { esp += 4; return; }
void sub_0008E8D0(void) { esp += 4; return; }
void sub_00090A27(void) { esp += 4; return; }
void sub_0009E127(void) { esp += 4; return; }
void sub_000A0BF0(void) { esp += 4; return; }
void sub_000A7410(void) { esp += 4; return; }
void sub_000E0080(void) { esp += 4; return; }
void sub_00200470(void) { esp += 4; return; }
void sub_0031ABD2(void) { esp += 4; return; }
void sub_0031AC0D(void) { esp += 4; return; }
void sub_003392F8(void) { esp += 4; return; }
void sub_003394FB(void) { esp += 4; return; }
void sub_00339506(void) { esp += 4; return; }
void sub_00339511(void) { esp += 4; return; }
void sub_0035B3B0(void) { esp += 4; return; }
void sub_00361BB4(void) { esp += 4; return; }

/**
 * sub_001FE1E0 - RenderWare linked-list traversal with callback
 * Original: 0x001FE1E0 (53 bytes) - rw_world_pipe_xbox
 *
 * Traverses a circular linked list and calls a comparison callback
 * for each element. Returns count of non-matching elements.
 *
 * Stack params: [esp+4]=list_head, [esp+8]=comp_func, [esp+C]=comp_arg
 *
 * BUG: The linked list at 0x41B44C can be uninitialized/corrupt,
 * causing an infinite loop. Added max iteration limit.
 */
void sub_001FE1E0(void)
{
    /* RW linked list traversal - skip entirely because lists can be
     * uninitialized/corrupt. Returns 0 (no matches found). */
    eax = 0;
    esp += 4; return;
}

/**
 * sub_0034D530 - D3D8LTCG rendering pipeline (STUB)
 *
 * Original: 0x0034D530-0x00360A54 (79 KB, D3D section)
 *
 * This is the Xbox D3D8's main rendering pipeline function - it processes
 * the NV2A push buffer, configures GPU state, and spin-waits for GPU
 * completion. Since the NV2A GPU doesn't exist in our D3D11 environment,
 * this function would hang forever in spin-wait loops reading GPU registers.
 *
 * Our D3D11 layer (d3d8_device.c) handles actual rendering separately.
 * This stub matches the original's ret 12 (pops 3 dword args + ret addr).
 */
/*
 * sub_001DB2C0 - Im2D vertex transform helper (STUB)
 *
 * Original: 0x001DB2C0 (not generated by recompiler)
 * Called via im2d function pointer table at 0x7592AC.
 * Similar to sub_001DB620/sub_001DB9D0 - vertex transformation.
 * Stub: return without action (im2d transforms not needed when
 * rendering through our D3D8→D3D11 bridge).
 */
void sub_001DB2C0(void)
{
    static uint32_t call_count = 0;
    call_count++;
    if (call_count <= 5 || (call_count % 10000) == 0)
        fprintf(stderr, "  [IM2D] sub_001DB2C0 (im2d transform) called #%u\n", call_count);
    esp += 4; return;
}

/* =================================================================
 * sub_0034D530 - D3D8LTCG rendering pipeline (LIVE PUSH BUFFER)
 *
 * Called from the RW display pipeline every frame. Instead of the
 * original 79K of NV2A push buffer generation code, we parse any
 * push buffer commands that sub_0034C2E0 or other D3D functions
 * wrote to our allocated push buffer, and translate them to D3D11.
 * ================================================================= */
volatile uint32_t g_d3d_render_count = 0;

extern void pgraph_d3d11_init(void);
extern void pgraph_d3d11_flush(void);
extern int pgraph_d3d11_method(int subchannel, uint32_t method, uint32_t param);

void parse_live_pushbuffer(void)
{
    static int inited = 0;
    if (!inited) { pgraph_d3d11_init(); inited = 1; }
    /* Read push buffer pointers directly from the known fixed addresses,
     * not from the device context (game's D3D init overwrites 0x35FB48). */
    uint32_t pb_base = MEM32(0x35D69C);   /* Push buffer base */
    uint32_t pb_write = MEM32(0x35D6A0);  /* Push buffer write pointer */

    static uint32_t diag_count = 0;
    diag_count++;
    if (diag_count <= 10 || (diag_count % 1000) == 0) {
        fprintf(stderr, "  [D3D8-LIVE] #%u pb_base=0x%08X pb_write=0x%08X delta=%d\n",
                diag_count, pb_base, pb_write, (int)(pb_write - pb_base));
    }

    if (pb_base == 0 || pb_write <= pb_base) return;

    uint32_t bytes_written = pb_write - pb_base;
    if (bytes_written < 8 || bytes_written > 4 * 1024 * 1024) return;

    uint32_t num_dwords = bytes_written / 4;
    uint32_t pos = 0;
    uint32_t method_count = 0;

    while (pos < num_dwords) {
        uint32_t header = MEM32(pb_base + pos * 4);
        if (header == 0) { pos++; continue; }

        /* Increasing method */
        if ((header & 0xE0030003) == 0) {
            uint32_t count = (header >> 18) & 0x7FF;
            uint32_t method = header & 0x1FFC;
            uint32_t subchan = (header >> 13) & 7;
            if (count == 0 || pos + 1 + count > num_dwords) { pos++; continue; }
            for (uint32_t i = 0; i < count; i++) {
                uint32_t param = MEM32(pb_base + (pos + 1 + i) * 4);
                pgraph_d3d11_method(subchan, method + i * 4, param);
                method_count++;
            }
            pos += 1 + count;
        }
        /* Non-increasing method */
        else if ((header & 0xE0030003) == 0x40000000) {
            uint32_t count = (header >> 18) & 0x7FF;
            uint32_t method = header & 0x1FFC;
            uint32_t subchan = (header >> 13) & 7;
            if (count == 0 || pos + 1 + count > num_dwords) { pos++; continue; }
            for (uint32_t i = 0; i < count; i++) {
                uint32_t param = MEM32(pb_base + (pos + 1 + i) * 4);
                pgraph_d3d11_method(subchan, method, param);
                method_count++;
            }
            pos += 1 + count;
        }
        else { pos++; }
    }

    /* Log raw push buffer contents for first few frames */
    if (method_count > 0 && diag_count <= 3) {
        fprintf(stderr, "  [D3D8-LIVE] Raw PB (%u dwords):", bytes_written / 4);
        for (uint32_t d = 0; d < bytes_written / 4 && d < 20; d++)
            fprintf(stderr, " %08X", MEM32(pb_base + d * 4));
        fprintf(stderr, "\n");
    }

    if (method_count > 0) {
        pgraph_d3d11_flush();
        static uint32_t log_count = 0;
        log_count++;
        if (log_count <= 5 || (log_count % 300) == 0) {
            fprintf(stderr, "  [D3D8-LIVE] Parsed %u methods from %u bytes of push buffer\n",
                    method_count, bytes_written);
        }
    }

    /* Reset write pointer for next frame */
    MEM32(0x35D6A0) = pb_base;
}

/* Forward declare the generated D3D8LTCG render pipeline */
extern void sub_0034D530_gen(void);

void sub_0034D530(void)
{
    g_d3d_render_count++;

    /* Force correct device pointer (gen code writes mirror variants) */
    MEM32(0x35FB48) = 0x0035D6A0;

    uint32_t pb_base = MEM32(0x35D69C);
    uint32_t dev = MEM32(0x35FB48);

    /* Reset device and global push buffer cursors to base before each call.
     * D3D8LTCG writes via device+0x00 (not via 0x35D6A0). */
    if (dev) {
        MEM32(dev + 0x00) = pb_base;
        MEM32(dev + 0x08) = pb_base;
    }
    MEM32(0x35D6A0) = pb_base;

    /* Call the generated D3D8LTCG code */
    sub_0034D530_gen();

    /* Sync device cursor to global write pointer for parser */
    uint32_t dev_pb_after = dev ? MEM32(dev + 0x00) : 0;
    if (dev_pb_after > pb_base) {
        MEM32(0x35D6A0) = dev_pb_after;
    }

    uint32_t bytes_written = dev_pb_after - pb_base;
    if (g_d3d_render_count <= 10 || (g_d3d_render_count % 5000) == 0)
        fprintf(stderr, "  [D3D8-RENDER] #%u: %u bytes written, ecx=0x%X\n",
                g_d3d_render_count, bytes_written, ecx);

    /* Parse push buffer commands (gen code output) and translate to D3D11 */
    parse_live_pushbuffer();
}

/* ── Im2D render overrides ──────────────────────────────────── */

/* RwIm2DVertex matches our rw_bridge.h struct: x,y,z,rhw,color,u,v (28 bytes) */
typedef struct Im2DVert {
    float x, y, z, rhw;
    uint32_t color;
    float u, v;
} Im2DVert;

extern int rw_bridge_im2d_render(int prim_type, const void *verts, int vert_count);
extern int rw_bridge_im2d_render_indexed(int prim_type, const void *verts,
                                          int vert_count, const uint16_t *indices,
                                          int index_count);

/**
 * sub_001DE900 - RwIm2DRenderPrimitive (OVERRIDE)
 *
 * Original: 0x001DE900 - 0x001DE92F (47 bytes, 12 insns)
 * cdecl: (void *verts, uint8_t primType, int32_t numVerts)
 *
 * The RW im2d render entry point. Game code calls this with pre-transformed
 * screen-space vertices (XYZRHW). Originally packs args and calls driver
 * table entry 0x0F (sub_001DBDE0) which goes through the Xbox D3D8LTCG
 * pipeline. We intercept here and route to rw_bridge_im2d_render().
 */
void sub_001DE900(void)
{
    /* Read cdecl params from recomp stack:
     * esp+4  = vertex buffer pointer (Xbox VA)
     * esp+8  = primitive type (byte, only low byte used)
     * esp+C  = vertex count (int32) */
    uint32_t vert_va   = MEM32(esp + 4);
    uint32_t prim_type = ZX8(MEM8(esp + 8));
    int32_t  num_verts = (int32_t)MEM32(esp + 0xC);

    static uint32_t call_count = 0;
    call_count++;

    if (num_verts <= 0 || num_verts > 4096) {
        if (call_count <= 10)
            fprintf(stderr, "  [IM2D-RENDER] #%u: bad vert count %d, skipping\n",
                    call_count, num_verts);
        eax = 0;
        esp += 4; return; /* ret */
    }

    /* Resolve vertex pointer to native memory */
    Im2DVert *native_verts = (Im2DVert *)((uintptr_t)vert_va + g_xbox_mem_offset);

    if (call_count <= 10 || (call_count % 5000) == 0) {
        fprintf(stderr, "  [IM2D-RENDER] #%u: type=%u verts=%d va=0x%08X",
                call_count, prim_type, num_verts, vert_va);
        if (num_verts > 0) {
            fprintf(stderr, " v0=(%.1f,%.1f,%.1f rhw=%.3f col=0x%08X)",
                    native_verts[0].x, native_verts[0].y, native_verts[0].z,
                    native_verts[0].rhw, native_verts[0].color);
        }
        fprintf(stderr, "\n");
    }

    /* Route through our D3D11 bridge */
    int ok = rw_bridge_im2d_render(prim_type, native_verts, num_verts);

    eax = ok ? 1 : 0;
    esp += 4; return; /* ret */
}

/**
 * sub_001DBDE0 - D3D8LTCG im2d render driver entry (OVERRIDE)
 *
 * Original: 0x001DBDE0 - 0x001DC138 (856 bytes, 290 insns)
 * Driver table entry 0x0F. Called with packed args:
 *   esp+4 = state_ptr, esp+8 = vert_ptr, esp+C = (primType << 8) + vertCount
 *
 * Since we override sub_001DE900 above, this should rarely be called.
 * Override it as safety net in case anything calls the driver entry directly.
 */
void sub_001DBDE0(void)
{
    /* Read packed args from driver call convention:
     * After the function's internal esp -= 0x14:
     *   esp+0x20 = packed (primType << 8) + vertCount
     *   esp+0x24 = vertex pointer
     * But since we're overriding from the entry, the stack is simpler:
     *   esp+4 = state_ptr, esp+8 = vert_ptr, esp+C = packed */
    uint32_t vert_va = MEM32(esp + 8);
    uint32_t packed  = MEM32(esp + 0xC);
    uint32_t prim_type = (packed >> 8) & 0xFF;
    int32_t  num_verts = packed & 0xFF;

    /* If vertCount > 255 the packed format may differ; try full value */
    if (num_verts == 0)
        num_verts = packed & 0xFFFF;

    static uint32_t call_count = 0;
    call_count++;
    if (call_count <= 10 || (call_count % 5000) == 0) {
        fprintf(stderr, "  [IM2D-DRV] #%u: entry 0x0F direct call, type=%u verts=%d\n",
                call_count, prim_type, num_verts);
    }

    if (num_verts > 0 && num_verts <= 4096 && vert_va != 0) {
        Im2DVert *native_verts = (Im2DVert *)((uintptr_t)vert_va + g_xbox_mem_offset);
        rw_bridge_im2d_render(prim_type, native_verts, num_verts);
    }

    eax = 0;
    esp += 4; return; /* ret */
}

/**
 * sub_000110E0 - Rendering context tick (STUB)
 *
 * Original: 0x000110E0 - 0x00011236 (342 bytes, 99 insns)
 *
 * This function is the Xbox rendering pipeline "tick". It:
 *   Part 1: Processes async callback from global list at 0x4AED9C
 *   Part 2: Iterates the rendering context's scene objects, calling
 *           vtable methods to sort/compare world pipes
 *
 * Part 2 makes hundreds of thousands of ICALLs (to sub_001F8860 and
 * invalid addresses like 0x12FE7CF0), eventually hanging in a spin-wait
 * on NV2A GPU registers. Since we use D3D11 for rendering, the Xbox
 * rendering pipeline tick is not needed.
 *
 * We keep Part 1 (async callback) since the loading pipeline's async
 * completion notifications may flow through it.
 *
 * Calling convention: cdecl, no params. Uses edi (set by caller to
 * rendering context pointer). Preserves ebx, esi.
 */
volatile uint32_t g_tick_110e0_count = 0;

void sub_000110E0(void)
{
    g_tick_110e0_count++;
    uint32_t tick_count = g_tick_110e0_count;

    /* Part 1: Process async callback (preserved for loading pipeline) */
    {
        uint32_t cb = MEM32(0x4AED9C);
        if (cb != 0) {
            uint32_t vtable = MEM32(cb);
            ecx = cb;
            uint32_t saved_esp = g_esp;
            PUSH32(esp, 0);
            RECOMP_ICALL_SAFE(MEM32(vtable + 4), saved_esp);
            if (tick_count <= 5)
                fprintf(stderr, "  [TICK] sub_000110E0 #%u: async callback at 0x%08X → eax=%u\n",
                        tick_count, MEM32(vtable + 4), eax);
        }
    }

    /* Part 2: Process task queue (file load completions).
     *
     * Original flow: checks queue at edi, creates reader via sub_001B33A0,
     * polls reader, sets completion flag when done. The reader uses RW
     * streaming pipeline which hangs on NV2A GPU registers.
     *
     * Simplified: skip the reader entirely. If there's an active queue entry,
     * immediately set its completion flag to 1 and advance the head.
     * This works because the resource data was already loaded into the
     * resource slots by init code before sub_00011240 was called.
     *
     * Queue layout (at edi):
     *   +0x788: head index (0-23 circular)
     *   +0x78C: tail/free index
     *   +0x790: version counter
     *   Entries at index*80: name string at +0, completion_flag_ptr at +0x40,
     *     resource at +0x44, param at +0x48, status at +0x4C (0 = empty)
     */
    /* Queue processing removed - completion flags now set directly
     * in sub_00011240 (gen patch) since the async RW stream reader
     * hangs on NV2A GPU registers. */

    /* Car physics integration with heading, speed, and drag.
     *
     * Fake physics body layout (at phys_ptr = 0x5FFF00):
     *   +0x08: forward acceleration (written by sub_000636D0)
     *   +0x0C: turn rate (written by sub_000636D0)
     *   +0x10: pos_x (world)
     *   +0x14: pos_y (world)
     *   +0x18: heading (radians, 0=up/north, CW positive)
     *   +0x1C: speed (scalar forward speed, units/s)
     *
     * Only integrate during gameplay to avoid garbage from
     * uninitialized accumulators during loading states.
     *
     * xemu Session 31 discovery: B8 stays at 5 during regular races!
     * Only crash mode sets B8=4. The real gameplay indicator is the
     * camera pointer: 0x4D45D0 = gameplay, 0x4D4008 = menus/loading.
     * Accept either B8==4 (crash mode) or camera==gameplay. */
    if (MEM32(0x4D53B8) == 4 || MEM32(0x4D5370) == 0x4D45D0) {
        uint32_t phys_ptr = MEM32(0x557880 + 0x1B4);
        if (phys_ptr > 0x100 && phys_ptr < 0x3FFFFFF) {
            /* Initialize physics on first state-4 entry, and respawn
             * when track changes (spawn coords updated by renderer).
             * g_force_respawn is set by fe_start_race to re-init after boot. */
            static int _state4_init = 0;
            static float _last_spawn_x = 0, _last_spawn_z = 0;
            {
                extern int g_force_respawn;
                if (g_force_respawn) {
                    _state4_init = 0;
                    _last_spawn_x = 0;
                    _last_spawn_z = 0;
                    g_force_respawn = 0;
                }
                int needs_init = !_state4_init;
                if (g_track_mode) {
                    if (g_track_spawn_x != _last_spawn_x || g_track_spawn_z != _last_spawn_z)
                        needs_init = 1;
                    _last_spawn_x = g_track_spawn_x;
                    _last_spawn_z = g_track_spawn_z;
                }
                if (needs_init) {
                    _state4_init = 1;
                    MEMF(phys_ptr + 0x08) = 0.0f;  /* accel */
                    MEMF(phys_ptr + 0x0C) = 0.0f;  /* turn rate */
                    if (g_track_mode) {
                        MEMF(phys_ptr + 0x10) = g_track_spawn_x;
                        MEMF(phys_ptr + 0x14) = g_track_spawn_z;
                        MEMF(phys_ptr + 0x18) = g_track_spawn_hdg;
                        fprintf(stderr, "  [PHY] Spawn on track: pos=(%.1f, %.1f) hdg=%.1f°\n",
                                g_track_spawn_x, g_track_spawn_z, g_track_spawn_hdg * 57.2958f);
                    } else {
                        MEMF(phys_ptr + 0x10) = 0.0f;
                        MEMF(phys_ptr + 0x14) = 0.0f;
                        MEMF(phys_ptr + 0x18) = 0.0f;
                    }
                    MEMF(phys_ptr + 0x1C) = 0.0f;  /* speed */
                }
            }

            float dt = MEMF(0x4AE1FC);
            if (dt <= 0.0f || dt > 0.2f) dt = 0.016f; /* sanity clamp */

            float accel     = MEMF(phys_ptr + 0x08);
            float turn_rate = MEMF(phys_ptr + 0x0C);
            float heading   = MEMF(phys_ptr + 0x18);
            float speed     = MEMF(phys_ptr + 0x1C);

            /* Apply acceleration */
            speed += accel * dt;

            /* Drag: proportional to speed, decelerates when not accelerating.
             * Very low drag on tracks for sustained high speed. */
            float drag = g_track_mode ? 0.15f : 0.8f;
            speed *= (1.0f - drag * dt);

            /* Clamp speed: max 500 units/s on track, 50 on procedural road */
            float max_speed = g_track_mode ? 500.0f : 50.0f;
            if (speed > max_speed) speed = max_speed;
            if (speed < -10.0f) speed = -10.0f;
            /* Kill very small speeds to prevent creeping */
            if (speed > -0.01f && speed < 0.01f) speed = 0.0f;

            /* Steering: turn rate scales with speed (can't turn while stopped).
             * At low speed, reduce turn. At high speed, slightly reduce too. */
            {
                float speed_factor;
                float abs_spd = speed < 0 ? -speed : speed;
                if (abs_spd < 0.5f)
                    speed_factor = abs_spd * 2.0f; /* ramp 0→1 over 0..0.5 */
                else if (abs_spd > 30.0f)
                    speed_factor = 1.0f - (abs_spd - 30.0f) * 0.01f; /* slight reduction */
                else
                    speed_factor = 1.0f;
                if (speed_factor < 0.0f) speed_factor = 0.0f;
                if (speed_factor > 1.0f) speed_factor = 1.0f;
                heading += turn_rate * speed_factor * dt;
            }

            /* Normalize heading to [-pi, pi] */
            while (heading > 3.14159265f) heading -= 6.28318530f;
            while (heading < -3.14159265f) heading += 6.28318530f;

            /* Position integration: move along heading direction.
             * heading=0 → north (pos_y increases), heading=pi/2 → east (pos_x increases) */
            float dx = sinf(heading) * speed * dt;
            float dy = cosf(heading) * speed * dt;
            float new_px = MEMF(phys_ptr + 0x10) + dx;
            float new_py = MEMF(phys_ptr + 0x14) + dy;

            /* Centripetal force from road curves (procedural road only).
             * On real tracks, car drives freely. */
            if (!g_track_mode) {
                float road_curve = MEMF(0x5FFD10);
                float centripetal = road_curve * speed * speed * 0.0003f;
                new_px += centripetal * dt;
            }

            /* Road edge collision: only for procedural road (no track loaded).
             * On real tracks, player drives freely in world space. */
            if (!g_track_mode) {
                float road_edge = 14.0f;
                if (new_px > road_edge) {
                    new_px = road_edge;
                    heading = -heading;
                    speed *= 0.5f;
                    MEMF(0x5FFD30) = 0.4f; /* spark timer */
                    MEM32(0x5FFD34) = 1; /* spark side: 1=right */
                    MEMF(0x5FFD28) = 1.0f;
                    MEMF(0x5FFD2C) = 0.0f;
                } else if (new_px < -road_edge) {
                    new_px = -road_edge;
                    heading = -heading;
                    speed *= 0.5f;
                    MEMF(0x5FFD30) = 0.4f; /* spark timer */
                    MEM32(0x5FFD34) = 0; /* spark side: 0=left */
                    MEMF(0x5FFD28) = 1.0f;
                    MEMF(0x5FFD2C) = 0.0f;
                }
            }

            MEMF(phys_ptr + 0x10) = new_px;
            MEMF(phys_ptr + 0x14) = new_py;

            /* Store updated heading and speed */
            MEMF(phys_ptr + 0x18) = heading;
            MEMF(phys_ptr + 0x1C) = speed;

            /* ── Write to real Xbox addresses (discovered via xemu Session 30) ──
             * The original game stores physics state at these addresses during
             * gameplay. Writing here lets any original recompiled code that reads
             * these addresses (rendering, AI, camera, HUD) work correctly.
             *
             * Car speed:        0x40FB18 (float, game units/s)
             * Race timer:       0x40FB20 (float, elapsed seconds)
             * Max speed cap:    0x40FD7C (float, top speed limit)
             * Car world matrix: 0x4D6850 (4x4 float, row-major)
             * Camera position:  0x4D9198/9C/A0 (float3 world XYZ)
             */
            MEMF(0x40FB18) = speed;

            /* Build 4x4 world transform matrix at 0x4D6850.
             * Row 0-2: rotation from heading, Row 3: position + w=1 */
            {
                float sh = sinf(heading);
                float ch = cosf(heading);
                /* Row 0: right vector */
                MEMF(0x4D6850 + 0x00) = ch;
                MEMF(0x4D6850 + 0x04) = 0.0f;
                MEMF(0x4D6850 + 0x08) = -sh;
                MEMF(0x4D6850 + 0x0C) = 0.0f;
                /* Row 1: up vector */
                MEMF(0x4D6850 + 0x10) = 0.0f;
                MEMF(0x4D6850 + 0x14) = 1.0f;
                MEMF(0x4D6850 + 0x18) = 0.0f;
                MEMF(0x4D6850 + 0x1C) = 0.0f;
                /* Row 2: forward vector */
                MEMF(0x4D6850 + 0x20) = sh;
                MEMF(0x4D6850 + 0x24) = 0.0f;
                MEMF(0x4D6850 + 0x28) = ch;
                MEMF(0x4D6850 + 0x2C) = 0.0f;
                /* Row 3: position + w=1 */
                MEMF(0x4D6850 + 0x30) = new_px;
                MEMF(0x4D6850 + 0x34) = 0.0f;  /* Y (height) */
                MEMF(0x4D6850 + 0x38) = new_py;
                MEMF(0x4D6850 + 0x3C) = 1.0f;
            }

            /* ── Write race state to real Xbox addresses ──
             * Discovered in xemu Session 31. These addresses are read by the
             * original game's HUD, AI, and camera systems. */
            {
                static float _race_timer = 120.0f;
                static uint32_t _lap = 1;
                static float _last_lap_y = 0.0f;
                static uint32_t _takedowns_this_race = 0;

                /* Race countdown timer (real game uses countdown, not elapsed) */
                _race_timer -= dt;
                if (_race_timer < 0.0f) _race_timer = 0.0f;
                MEMF(0x411BF8) = _race_timer;

                /* Lap tracking: increment when player crosses origin in +Y dir */
                if (new_py > 0.0f && _last_lap_y <= 0.0f && speed > 5.0f) {
                    _lap++;
                    _race_timer = 120.0f; /* reset timer on lap */
                }
                _last_lap_y = new_py;
                MEM32(0x411BDC) = _lap;
                MEM32(0x411B30) = _lap; /* duplicate */

                /* Player position (always 1st in our solo mode) */
                MEM32(0x550520) = 1;

                /* Boost meter: mirror our 0-100 value to the real address.
                 * Real game uses much larger values (100-1800+), so scale up. */
                MEMF(0x40FD7C) = MEMF(0x5FFD08) * 10.0f;

                /* Race control block sane defaults */
                MEM32(0x411B20) = 1;    /* active flag */
                MEM32(0x411BE0) = 5;    /* race type */
                MEM32(0x411BE8) = 2;    /* total laps */
            }

            /* ── Traffic obstacles ──────────────────────────────────── */
            /* Obstacle array at 0x5FFE00: 12 slots × 16 bytes each.
             * Layout per slot: +0 pos_x (float), +4 pos_y (float),
             *   +8 speed (float, negative=oncoming), +C flags (uint32_t)
             * Flags: bit 0=active, bit 1=oncoming
             * Slots 0-7: same-direction traffic (positive speed)
             * Slots 8-11: oncoming traffic (negative speed, left lanes) */
            {
                #define OBS_BASE   0x5FFE00
                #define OBS_COUNT  12
                #define OBS_SAME   8
                #define OBS_SIZE   16
                #define OBS_ADDR(i, off) (OBS_BASE + (i) * OBS_SIZE + (off))
                static int _obs_init = 0;
                static uint32_t _takedown_count = 0;
                static uint32_t _obs_seed = 12345;
                int oi;

                /* Simple LCG PRNG for obstacle placement */
                #define OBS_RAND() (_obs_seed = _obs_seed * 1103515245 + 12345, (_obs_seed >> 16) & 0x7FFF)

                /* Difficulty scaling */
                float difficulty = new_py * 0.0001f;
                if (difficulty > 1.0f) difficulty = 1.0f;
                if (difficulty < 0.0f) difficulty = 0.0f;
                float spawn_base = 80.0f - difficulty * 40.0f;
                float spawn_range = 60.0f - difficulty * 20.0f;
                float speed_range = 8.0f + difficulty * 10.0f;

                /* Store distance for HUD display */
                MEM32(0x5FFD14) = (uint32_t)new_py;

                /* Initialize obstacles on first call.
                 * Skip traffic on real tracks - traffic math assumes straight road. */
                if (!_obs_init) {
                    _obs_init = 1;
                    if (g_track_mode) {
                        /* Track mode: no traffic (road geometry is too complex) */
                        for (oi = 0; oi < OBS_COUNT; oi++)
                            MEM32(OBS_ADDR(oi, 0xC)) = 0; /* inactive */
                    } else {
                        /* Same-direction traffic (slots 0-7) */
                        for (oi = 0; oi < OBS_SAME; oi++) {
                            float lane = ((float)(OBS_RAND() % 5) - 2.0f) * 5.0f;
                            float ahead = 30.0f + (float)(OBS_RAND() % 80);
                            MEMF(OBS_ADDR(oi, 0)) = lane;
                            MEMF(OBS_ADDR(oi, 4)) = new_py + ahead;
                            MEMF(OBS_ADDR(oi, 8)) = 3.0f + (float)(OBS_RAND() % 5);
                            MEM32(OBS_ADDR(oi, 0xC)) = 1; /* active, same-dir */
                        }
                        /* Oncoming traffic (slots 8-11): left lanes, negative speed */
                        for (oi = OBS_SAME; oi < OBS_COUNT; oi++) {
                            float lane = -5.0f - (float)(OBS_RAND() % 3) * 3.0f;
                            float ahead = 60.0f + (float)(OBS_RAND() % 100);
                            MEMF(OBS_ADDR(oi, 0)) = lane;
                            MEMF(OBS_ADDR(oi, 4)) = new_py + ahead;
                            MEMF(OBS_ADDR(oi, 8)) = -(10.0f + (float)(OBS_RAND() % 8));
                            MEM32(OBS_ADDR(oi, 0xC)) = 3; /* active + oncoming */
                        }
                    }
                }

                /* Update and collide each obstacle */
                for (oi = 0; oi < OBS_COUNT; oi++) {
                    uint32_t flags = MEM32(OBS_ADDR(oi, 0xC));
                    if ((flags & 1) == 0) continue; /* inactive */
                    int is_oncoming = (flags & 2) != 0;

                    float ox = MEMF(OBS_ADDR(oi, 0));
                    float oy = MEMF(OBS_ADDR(oi, 4));
                    float os = MEMF(OBS_ADDR(oi, 8));

                    /* Move obstacle at its own speed */
                    oy += os * dt;
                    MEMF(OBS_ADDR(oi, 4)) = oy;

                    /* Same-dir traffic: lane changes + braking AI.
                     * Sine drift for smooth lane-change behavior.
                     * Brake when player approaches from behind in same lane. */
                    if (!is_oncoming) {
                        float drift_phase = oy * 0.03f + (float)oi * 1.57f;
                        float drift = sinf(drift_phase) * 2.0f * dt;
                        ox += drift;
                        /* Clamp to road bounds */
                        if (ox > 12.0f) ox = 12.0f;
                        if (ox < -12.0f) ox = -12.0f;
                        MEMF(OBS_ADDR(oi, 0)) = ox;

                        /* Brake if player is approaching from behind */
                        float dx_lane = new_px - ox;
                        float dy_behind = oy - new_py;
                        if (dx_lane > -4.0f && dx_lane < 4.0f &&
                            dy_behind > 0.0f && dy_behind < 20.0f &&
                            speed > os + 2.0f) {
                            /* Slow down gradually */
                            os -= 8.0f * dt;
                            if (os < 1.0f) os = 1.0f;
                            MEMF(OBS_ADDR(oi, 8)) = os;
                        }
                    }

                    /* Recycle if too far behind (or ahead for oncoming that passed) */
                    if (oy < new_py - 60.0f || (is_oncoming && oy < new_py - 30.0f)) {
                        if (is_oncoming) {
                            float lane = -5.0f - (float)(OBS_RAND() % 3) * 3.0f;
                            float ahead = spawn_base + 40.0f + (float)(OBS_RAND() % (int)(spawn_range + 1.0f));
                            MEMF(OBS_ADDR(oi, 0)) = lane;
                            MEMF(OBS_ADDR(oi, 4)) = new_py + ahead;
                            MEMF(OBS_ADDR(oi, 8)) = -(10.0f + (float)(OBS_RAND() % (int)(speed_range + 1.0f)));
                        } else {
                            float lane = ((float)(OBS_RAND() % 5) - 2.0f) * 5.0f;
                            float ahead = spawn_base + (float)(OBS_RAND() % (int)(spawn_range + 1.0f));
                            MEMF(OBS_ADDR(oi, 0)) = lane;
                            MEMF(OBS_ADDR(oi, 4)) = new_py + ahead;
                            MEMF(OBS_ADDR(oi, 8)) = 3.0f + (float)(OBS_RAND() % (int)(speed_range + 1.0f));
                        }
                        continue;
                    }

                    /* AABB collision: car is ~4.5×2, obstacle is ~4×2 */
                    float rel_x = new_px - ox;
                    float rel_y = new_py - oy;
                    float abs_rx = rel_x < 0 ? -rel_x : rel_x;
                    float abs_ry = rel_y < 0 ? -rel_y : rel_y;
                    if (abs_rx < 3.0f && abs_ry < 4.5f) {
                        if (is_oncoming) {
                            /* CRASH! Head-on with oncoming = devastating.
                             * Lose most speed, red screen shake, no boost.
                             * Reset multiplier. */
                            speed *= 0.15f; /* lose 85% speed */
                            MEMF(phys_ptr + 0x1C) = speed;
                            MEMF(0x5FFD18) = 1.0f; /* screen shake timer */
                            MEMF(0x5FFD04) = 0.8f; /* red flash */
                            MEMF(0x5FFD28) = 1.0f; /* reset multiplier */
                            MEMF(0x5FFD2C) = 0.0f; /* reset combo timer */
                            /* Respawn oncoming car */
                            float lane = -5.0f - (float)(OBS_RAND() % 3) * 3.0f;
                            MEMF(OBS_ADDR(oi, 0)) = lane;
                            MEMF(OBS_ADDR(oi, 4)) = new_py + spawn_base + 60.0f + (float)(OBS_RAND() % (int)(spawn_range + 1.0f));
                            MEMF(OBS_ADDR(oi, 8)) = -(10.0f + (float)(OBS_RAND() % (int)(speed_range + 1.0f)));
                        } else {
                            /* TAKEDOWN! Same-direction rear-end = boost reward */
                            _takedown_count++;
                            float lane = ((float)(OBS_RAND() % 5) - 2.0f) * 5.0f;
                            MEMF(OBS_ADDR(oi, 0)) = lane;
                            MEMF(OBS_ADDR(oi, 4)) = new_py + spawn_base + 20.0f + (float)(OBS_RAND() % (int)(spawn_range + 1.0f));
                            MEMF(OBS_ADDR(oi, 8)) = 3.0f + (float)(OBS_RAND() % (int)(speed_range + 1.0f));

                            speed += 5.0f;
                            if (speed > 50.0f) speed = 50.0f;
                            MEMF(phys_ptr + 0x1C) = speed;

                            MEM32(0x5FFD00) = _takedown_count;
                            MEMF(0x5FFD04) = 0.5f; /* white flash */

                            float boost = MEMF(0x5FFD08);
                            boost += 25.0f;
                            if (boost > 100.0f) boost = 100.0f;
                            MEMF(0x5FFD08) = boost;

                            /* Score: takedown = 500 * multiplier */
                            {
                                float mult = MEMF(0x5FFD28);
                                if (mult < 1.0f) mult = 1.0f;
                                uint32_t score = MEM32(0x5FFD24);
                                score += (uint32_t)(500.0f * mult);
                                MEM32(0x5FFD24) = score;
                                mult += 0.5f;
                                if (mult > 8.0f) mult = 8.0f;
                                MEMF(0x5FFD28) = mult;
                                MEMF(0x5FFD2C) = 3.0f; /* combo timer */
                            }
                        }
                    }
                    /* Near-miss detection */
                    else if (abs_rx < 5.0f && abs_ry < 6.0f && abs_ry > 2.0f
                             && speed > 5.0f) {
                        float boost = MEMF(0x5FFD08);
                        /* Oncoming near-miss fills faster */
                        float fill = is_oncoming ? 8.0f : 5.0f;
                        boost += fill * dt * 10.0f;
                        if (boost > 100.0f) boost = 100.0f;
                        MEMF(0x5FFD08) = boost;

                        /* Score: near-miss = 50 * multiplier (oncoming = 100) */
                        {
                            float mult = MEMF(0x5FFD28);
                            if (mult < 1.0f) mult = 1.0f;
                            uint32_t score = MEM32(0x5FFD24);
                            float pts = is_oncoming ? 100.0f : 50.0f;
                            score += (uint32_t)(pts * mult * dt * 5.0f);
                            MEM32(0x5FFD24) = score;
                            mult += 0.1f * dt;
                            if (mult > 8.0f) mult = 8.0f;
                            MEMF(0x5FFD28) = mult;
                            MEMF(0x5FFD2C) = 3.0f; /* combo timer */
                        }
                    }
                }
                #undef OBS_BASE
                #undef OBS_COUNT
                #undef OBS_SAME
                #undef OBS_SIZE
                #undef OBS_ADDR
                #undef OBS_RAND
            }

            /* Boost activation: Shift key drains boost for extra speed */
            {
                float boost = MEMF(0x5FFD08);
                uint32_t boost_active = MEM32(0x5FFD0C);
                if (boost_active && boost > 0.0f && speed > 1.0f) {
                    /* Drain boost meter */
                    float drain = 30.0f * dt; /* empty in ~3.3 seconds */
                    boost -= drain;
                    if (boost < 0.0f) boost = 0.0f;
                    MEMF(0x5FFD08) = boost;
                    /* Apply speed boost: +50% max speed */
                    speed += 20.0f * dt;
                    if (speed > 75.0f) speed = 75.0f; /* boosted max speed */
                    MEMF(phys_ptr + 0x1C) = speed;
                }
            }

            /* Checkpoint system: every 500 world units */
            {
                uint32_t last_cp = MEM32(0x5FFD1C);
                uint32_t current_dist = (uint32_t)new_py;
                uint32_t next_cp = last_cp + 500;
                if (current_dist >= next_cp && new_py > 100.0f) {
                    MEM32(0x5FFD1C) = (current_dist / 500) * 500;
                    MEMF(0x5FFD20) = 1.5f; /* checkpoint flash timer */
                    /* Reward: small boost fill */
                    float boost = MEMF(0x5FFD08);
                    boost += 15.0f;
                    if (boost > 100.0f) boost = 100.0f;
                    MEMF(0x5FFD08) = boost;
                    /* Score: checkpoint = 1000 * multiplier */
                    {
                        float mult = MEMF(0x5FFD28);
                        if (mult < 1.0f) mult = 1.0f;
                        uint32_t score = MEM32(0x5FFD24);
                        score += (uint32_t)(1000.0f * mult);
                        MEM32(0x5FFD24) = score;
                    }
                }
            }

            /* Decrement flash and shake timers */
            {
                float flash = MEMF(0x5FFD04);
                if (flash > 0.0f) {
                    flash -= dt;
                    if (flash < 0.0f) flash = 0.0f;
                    MEMF(0x5FFD04) = flash;
                }
                float shake = MEMF(0x5FFD18);
                if (shake > 0.0f) {
                    shake -= dt;
                    if (shake < 0.0f) shake = 0.0f;
                    MEMF(0x5FFD18) = shake;
                }
                float cp_flash = MEMF(0x5FFD20);
                if (cp_flash > 0.0f) {
                    cp_flash -= dt;
                    if (cp_flash < 0.0f) cp_flash = 0.0f;
                    MEMF(0x5FFD20) = cp_flash;
                }
                /* Spark timer */
                float spark = MEMF(0x5FFD30);
                if (spark > 0.0f) {
                    spark -= dt;
                    if (spark < 0.0f) spark = 0.0f;
                    MEMF(0x5FFD30) = spark;
                }
                /* Combo/multiplier decay */
                float combo_timer = MEMF(0x5FFD2C);
                if (combo_timer > 0.0f) {
                    combo_timer -= dt;
                    if (combo_timer < 0.0f) combo_timer = 0.0f;
                    MEMF(0x5FFD2C) = combo_timer;
                } else {
                    /* When combo expires, decay multiplier toward 1.0 */
                    float mult = MEMF(0x5FFD28);
                    if (mult > 1.0f) {
                        mult -= 0.5f * dt;
                        if (mult < 1.0f) mult = 1.0f;
                        MEMF(0x5FFD28) = mult;
                    }
                }
            }
        }
    }

    /* Diagnostic: print state, timing, and simulation data */
    if (tick_count <= 20 || (tick_count % 500 == 0)) {
        uint32_t phys_ptr = MEM32(0x557880 + 0x1B4);
        float spd = 0.0f, hdg = 0.0f;
        float pos_x = 0.0f, pos_y = 0.0f;
        if (phys_ptr > 0x100 && phys_ptr < 0x3FFFFFF) {
            pos_x = MEMF(phys_ptr + 0x10);
            pos_y = MEMF(phys_ptr + 0x14);
            hdg   = MEMF(phys_ptr + 0x18);
            spd   = MEMF(phys_ptr + 0x1C);
        }
        fprintf(stderr, "  [TICK] #%u: game=%u dt=%.4f spd=%.2f hdg=%.1f° pos=(%.1f,%.1f) icalls=%llu\n",
                tick_count, MEM32(0x4D53B8), MEMF(0x4AE1FC),
                spd, hdg * 57.2958f, pos_x, pos_y,
                (unsigned long long)g_icall_count);
    }

    /* Signal "rendering complete" to the game state machine.
     * The gate flag at 0x4D53BC (= ebp + 0x2E21C) controls whether the
     * game_state can transition. It's set to 1 when MEM8(0x4D53BE) is
     * non-zero (checked in sub_000165F0 at loc_0001663B). In the original
     * game, the RW rendering pipeline sets this after completing a frame.
     * Since we stub rendering, set it here so the game can advance states. */
    MEM8(0x4D53BE) = 1;

    /* Pump the Windows message loop and present a D3D frame.
     * The original Xbox rendering pipeline (Part 2) is stubbed because it
     * hangs on NV2A GPU registers. Instead, we call our D3D11 frame pump
     * which clears to a solid color and presents. This runs at ~60fps
     * (throttled inside game_frame_pump). */
    {
        extern void game_frame_pump(void);
        game_frame_pump();
    }

    esp += 4; return; /* ret: pop dummy return address */
}

/**
 * sub_00135040 - Audio/streaming subsystem init (STUB)
 *
 * Original: 0x00135040 - 0x00135240 (512 bytes, 134 insns)
 * Category: game_audio
 *
 * This function initializes the game's audio/streaming subsystem at edi+0x40B310.
 * It calls many RW functions (sub_001F7150, sub_001F77C0, etc.) and enters a
 * spin-loop in the RW rendering pipe iteration (0x12FE7CF0 / sub_001F8860 pattern).
 *
 * Stubbed because the audio subsystem isn't needed for initial rendering.
 * The original function uses edi (set by caller to 0x40B310) and sets up
 * various fields at edi+offsets.
 */
void sub_00135040(void)
{
    /* edi = audio subsystem struct base (set by caller, typically 0x40B310)
     *
     * The original function:
     *   1. Calls sub_001526A0 (resource pipeline setup)
     *   2. Calls sub_001CA350 (RW audio init) — this sets 0x73A190/0x73A194
     *   3. Calls sub_001F7150 x2 (RW pipeline alloc) — returns handles
     *   4. Sets up buffer pointers at edi+0x520/0x524/0x528
     *   5. Enters RW pipeline iteration (spin-loop) — we skip this
     *   6. Initializes resource slots at edi+0x6B50-0x6B60
     *
     * Our override: set the critical fields directly and mark audio as ready.
     * The DirectSound device already exists (dsound_device.c), so the game's
     * audio state machine (sub_0013EA20) can proceed.
     */

    /* edi is the audio subsystem base — check it's valid */
    if (edi < 0x10000 || edi > 0x4000000) {
        fprintf(stderr, "  [AUDIO-INIT] sub_00135040: invalid edi=0x%08X, skipping\n", edi);
        esp += 4; return;
    }

    fprintf(stderr, "  [AUDIO-INIT] sub_00135040: initializing audio subsystem at 0x%08X\n", edi);

    /* Set audio enable flags — sub_0013EA20 checks (0x73A190 | 0x73A194) != 0 */
    MEM32(0x73A190) = edi;       /* Audio subsystem pointer A */
    MEM32(0x73A194) = edi + 4;   /* Audio subsystem pointer B */

    /* Set up buffer pointers (matches gen code lines 180246-180264) */
    MEM32(edi + 0x520) = 0x411E80;  /* Audio buffer base */
    MEM32(edi + 0x524) = 0x411E9C;  /* Streaming buffer base */
    MEM32(edi + 0x528) = 1;         /* Initialized flag */

    /* Initialize audio buffer memory */
    MEM32(0x411E88) = 0;  /* RW pipe handle A (would come from sub_001F7150) */
    MEMF(0x411E84) = 0.0f;
    MEM32(0x411E90) = 0;
    MEM32(0x411EA4) = 0;  /* RW pipe handle B */
    MEMF(0x411EA0) = 0.0f;
    MEM32(0x411EAC) = 0;

    /* Clear resource slots */
    MEM32(edi + 0x6B50) = 0;
    MEM32(edi + 0x6B54) = 0;
    MEM32(edi + 0x6B58) = 0;
    MEM32(edi + 0x6B5C) = 0;
    MEM32(edi + 0x6B60) = 1;  /* Resource init complete flag */

    /* Mark init flag so we don't re-enter */
    MEM8(edi + 0x2E04) = 1;

    /* Zero the audio state controller fields that sub_0013EA20 checks */
    MEM8(edi + 4) = 0;

    /* Initialize the audio context pointer (used all over the audio code) */
    MEM32(0x73A19C) = edi;

    /* Set initial audio state values that sub_0013EA20 needs */
    MEM8(0x411E74) = 0;    /* Master volume byte */
    MEMF(0x3EBFCC) = 0.0f; /* Volume accumulator */
    MEMF(0x4A1EF0) = 0.0f; /* Volume output */

    fprintf(stderr, "  [AUDIO-INIT] Audio subsystem initialized: enable=0x%08X/0x%08X ctx=0x%08X\n",
            MEM32(0x73A190), MEM32(0x73A194), MEM32(0x73A19C));

    esp += 4; return;
}

/**
 * sub_00018BB0 - Resource slot polling
 * Original checks a version counter at [ecx+4] against edx (global version).
 * Since we defer worker threads (they can't run synchronously), the version
 * field is never updated, causing a permanent mismatch.
 *
 * This override skips the version check: if the slot status [ecx] == 0
 * (free) and the resource pointer [ecx+C] is non-zero, return it.
 * Otherwise return 0 (not ready).
 *
 * Original logic:
 *   if ([ecx+4] < 0) skip version check
 *   if ([ecx+4] != edx) return 0
 *   if ([ecx] != 0) return 0
 *   return [ecx+C], set [ecx] = -1
 */
void sub_00018BB0(void)
{
    uint32_t slot_status = MEM32(ecx);
    uint32_t resource_ptr = MEM32(ecx + 0xC);
    static int _18bb0_count = 0;

    if (slot_status == 0 && resource_ptr != 0) {
        /* Resource is available - return it and mark slot consumed */
        MEM32(ecx) = 0xFFFFFFFFu;
        eax = resource_ptr;
        if (_18bb0_count < 30)
            fprintf(stderr, "  [18BB0] ecx=0x%08X status=%u res=0x%08X edx=0x%08X → RETURN 0x%08X\n",
                    ecx, slot_status, resource_ptr, edx, eax);
    } else {
        eax = 0;
        if (_18bb0_count < 30)
            fprintf(stderr, "  [18BB0] ecx=0x%08X status=0x%08X res=0x%08X edx=0x%08X → NULL\n",
                    ecx, slot_status, resource_ptr, edx);
    }
    _18bb0_count++;
    esp += 4; return; /* ret */
}

/**
 * sub_00020930 - RW resource pointer fixup (STUB)
 *
 * Original: 0x00020930 - 0x00020961 (49 bytes)
 *
 * This function does pointer fixup on RW binary data: reads a count
 * and offset table from the resource, adds the base address to each
 * offset to create absolute pointers. Since worker threads were deferred
 * and never processed the raw resource data, the count/offset fields
 * contain garbage, causing the fixup loop to access invalid memory.
 *
 * Stub: stores the resource pointer at the destination and returns 1.
 */
void sub_00020930(void)
{
    /* MEM32(ecx) = eax: store resource ptr at destination */
    MEM32(ecx) = eax;
    SET_LO8(eax, 1);
    esp += 4; return; /* ret */
}

/**
 * sub_00159710 - RW resource pointer relocation (FULL IMPLEMENTATION)
 *
 * Original: 0x00159710 - 0x0015974F (63 bytes, 28 insns)
 * Calls sub_001596B0 for each item.
 *
 * Adjusts internal pointers in a loaded RW resource by a delta:
 *   esi = struct pointer, eax = delta (base address to add)
 *   +0: marker/flags (untouched)
 *   +4: pointer to item array (adjusted by delta)
 *   +8: count of items
 *   +0xC: pointer to section 2 data (adjusted by delta)
 *
 * Item array has entries at stride 8 bytes: {uint32 offset, uint32 count}
 * Each item's offset field is relocated, then nested structures are relocated:
 *   - Sub-entries at stride 0x18 from the relocated base, field +0x10 relocated
 *   - Sub-sub-entries at stride 0x40, field +0x34 relocated
 */
void sub_00159710(void)
{
    uint32_t delta = eax;
    int32_t item_count, i;

    /* 1. Relocate the item array pointer */
    MEM32(esi + 4) = MEM32(esi + 4) + delta;

    /* 2. Get item count */
    item_count = (int32_t)MEM32(esi + 8);

    fprintf(stderr, "  [RELOC] sub_00159710: esi=0x%08X delta=0x%08X items=%d\n",
            esi, delta, item_count);

    /* 3. Process each item via the sub_001596B0 algorithm */
    if (item_count > 0 && item_count < 1000) {
        for (i = 0; i < item_count; i++) {
            uint32_t item_va = MEM32(esi + 4) + (uint32_t)(i * 8);
            uint32_t entry_offset, entry_count;
            int32_t j;

            /* Read and relocate the entry data pointer */
            entry_offset = MEM32(item_va);
            entry_count = (int32_t)MEM32(item_va + 4);
            entry_offset += delta;
            MEM32(item_va) = entry_offset;

            fprintf(stderr, "  [RELOC]   item[%d]: va=0x%08X ptr=0x%08X count=%d\n",
                    i, item_va, entry_offset, entry_count);

            /* Process sub-entries (stride 0x18) */
            if (entry_count > 0 && entry_count < 10000) {
                for (j = 0; j < entry_count; j++) {
                    uint32_t sub_base = entry_offset + (uint32_t)(j * 0x18);
                    uint32_t nested_ptr;
                    int32_t sub_count, k;

                    /* Relocate the nested pointer at +0x10 */
                    nested_ptr = MEM32(sub_base + 0x10);
                    nested_ptr += delta;
                    MEM32(sub_base + 0x10) = nested_ptr;

                    /* Get sub-sub-entry count at +0x14 */
                    sub_count = (int32_t)MEM32(sub_base + 0x14);

                    /* Process sub-sub-entries (stride 0x40) */
                    if (sub_count > 0 && sub_count < 10000) {
                        for (k = 0; k < sub_count; k++) {
                            uint32_t deep_ptr_va = nested_ptr + (uint32_t)(k * 0x40) + 0x34;
                            MEM32(deep_ptr_va) = MEM32(deep_ptr_va) + delta;
                        }
                    }
                }
            }
        }
    }

    /* 4. Relocate the section 2 pointer */
    MEM32(esi + 0xC) = MEM32(esi + 0xC) + delta;

    fprintf(stderr, "  [RELOC]   section2 ptr: 0x%08X\n", MEM32(esi + 0xC));

    /* 5. Set global 0x4D1FE8 to point to the relocated resource.
     * This address is read by sub_00157680 (PrgData linker) and other code
     * that expects a parsed/relocated resource structure. Normally set by
     * the RW stream parser which doesn't run. */
    MEM32(0x4D1FE8) = esi;
    fprintf(stderr, "  [RELOC]   set MEM32(0x4D1FE8) = 0x%08X (PrgData root)\n", esi);

    esp += 4; return; /* ret */
}

/**
 * sub_0001BE60 - Track/scene setup (STUB)
 *
 * Original: 0x0001BE60 - 0x0001BFC8 (360 bytes, 86 insns)
 * Category: game_engine
 *
 * Initializes track rendering data: processes materials, textures,
 * and geometry from the RW world structure. Calls 13 sub-functions
 * including sub_0001C340 (scene processing, 485 insns) which iterates
 * through world geometry data.
 *
 * Stubbed because the RW world data at 0x4D1FE8 isn't properly loaded
 * (worker threads deferred). sub_0001C340 hangs reading garbage
 * geometry counts from unprocessed RW structures.
 */
void sub_0001BE60(void)
{
    fprintf(stderr, "  [STUB] sub_0001BE60 (track/scene setup) - skipped\n");
    esp += 4; return; /* ret */
}

/**
 * sub_00135240 - Audio streaming setup (STUB)
 *
 * Original: 0x00135240 - 0x00135350 (272 bytes, 75 insns)
 * Category: game_audio
 *
 * Polls resource slots for audio data, then initializes the streaming
 * audio subsystem (DirectSound buffers, XMA decoders, etc.).
 * Depends on sub_00135040 having run first (which we stubbed).
 * Returns non-zero LO8 on success.
 */
void sub_00135240(void)
{
    fprintf(stderr, "  [STUB] sub_00135240 (audio streaming setup) - skipped\n");
    SET_LO8(eax, 1);
    esp += 4; return; /* ret */
}

/**
 * sub_001B33A0 - RW stream reader (OVERRIDE)
 *
 * Original: 0x001B33A0 - 0x001B3410 (112 bytes, 56 insns)
 * Category: rw_core
 * CC: stdcall, 0 params (uses esi = stream object)
 *
 * Iterates over items in an RW stream object at ESI:
 *   ESI+0x00: vtable pointer (or data array base)
 *   ESI+0x08: state flag (output: 3 = success)
 *   ESI+0x0C: item count
 *
 * The original function calls vtable methods for each item which walk
 * GPU-allocated memory (0x92-0x93 range). Our override marks the stream
 * as complete without walking those vtables.
 *
 * This allows the track environment loader (sub_00062BD0) and other
 * RW streaming consumers to progress past the "read stream" phase.
 */
void sub_001B33A0(void)
{
    static int _1B33A0_count = 0;
    _1B33A0_count++;

    /* Validate esi points to a valid stream object */
    if (esi < 0x10000 || esi > 0x4000000) {
        if (_1B33A0_count <= 5)
            fprintf(stderr, "  [RW-STREAM] sub_001B33A0: invalid esi=0x%08X, skipping\n", esi);
        eax = 0;
        esp += 4; esp += 12; return;
    }

    uint32_t count = MEM32(esi + 0xC);
    uint32_t state = MEM32(esi + 8);

    if (_1B33A0_count <= 10 || (_1B33A0_count % 500) == 0)
        fprintf(stderr, "  [RW-STREAM] sub_001B33A0 #%d: esi=0x%08X count=%u state=%u → complete\n",
                _1B33A0_count, esi, count, state);

    /* Mark stream as successfully completed (state = 3) */
    MEM32(esi + 8) = 3;

    eax = 0;
    esp += 4;  /* pop return address */
    esp += 12; /* ret 12: clean 3 params from stack */
    return;
}

/**
 * sub_00062BD0 - Track environment loader (STUB)
 *
 * Original: 0x00062BD0 - 0x00062D60 (400 bytes, 119 insns)
 * Category: game_engine
 *
 * Loads track environment data via resource slot 0x3F9DB4 and processes
 * it through a multi-state internal state machine. The first state calls
 * sub_001B33A0 (RW stream reader) which hangs on NV2A GPU registers.
 *
 * Stubbed to return 1 (success) since the track environment can't be
 * loaded without the RW streaming pipeline.
 */
void sub_00062BD0(void)
{
    static int _62bd0_count = 0;
    if (_62bd0_count < 3) {
        /* Dump diagnostics about what track the game wants to load */
        const char *path_buf = (const char *)XBOX_PTR(0x38A26C);
        uint32_t track_slot = MEM32(0x3F9DB4);
        uint32_t fname_ptr = MEM32(0x3FA644);
        fprintf(stderr, "  [TRACK] sub_00062BD0: edi=0x%08X state=0x%08X\n",
                edi, MEM32(edi + 0x6B4));
        fprintf(stderr, "  [TRACK]   path@0x38A26C: '%.64s'\n", path_buf);
        fprintf(stderr, "  [TRACK]   0x3F9DB4=%u  0x3FA644=0x%08X\n",
                track_slot, fname_ptr);
        if (fname_ptr > 0x100 && fname_ptr < 0x4000000) {
            const char *fn = (const char *)XBOX_PTR(fname_ptr);
            fprintf(stderr, "  [TRACK]   filename@0x3FA644: '%.64s'\n", fn);
        }
    }
    _62bd0_count++;
    SET_LO8(eax, 1);
    esp += 4; return; /* ret */
}

/**
 * sub_0004DD00 - Pipeline/material name lookup (STUB)
 *
 * Original: 0x0004DD00 - 0x0004DEA6 (422 bytes, 137 insns)
 * Category: game_engine
 *
 * Looks up named rendering pipelines and materials in the RW world.
 * Iterates through world entries (MEM32(0x4D1FE0)+8 = count) searching
 * by name (via sub_00244C51/strcmp). Populates tables at 0x460770,
 * 0x4607C8, 0x460848 with pointers to found entries.
 *
 * Stubbed because RW world structure isn't properly initialized
 * (worker threads deferred, scene setup skipped). The world entry
 * count at 0x4D1FE0+8 may be garbage, causing infinite loops.
 */
void sub_0004DD00(void)
{
    fprintf(stderr, "  [STUB] sub_0004DD00 (pipeline/material lookup) - skipped\n");
    esp += 4; return; /* ret */
}

/**
 * sub_00221F20 - RW hash table lookup (SAFE OVERRIDE)
 *
 * Original: 0x00221F20 - 0x00221F80 (96 bytes, 49 insns)
 * Category: rw_world_pipe_xbox
 *
 * Hash table entry removal: reads table_size from ecx+0x10,
 * uses it as divisor for index probing. When RW pipeline tables
 * aren't initialized, table_size=0, causing division-by-zero.
 *
 * This override adds a guard: if table_size is 0, return 0.
 * Otherwise performs the original hash table lookup logic.
 *
 * Calling convention: stdcall-like, 1 param on stack (ecx via stack)
 *   [esp+4] = pointer to hash table structure
 */
void sub_00221F20(void)
{
    /* ecx = [esp+4] (param) */
    ecx = MEM32(esp + 4);

    /* Original early-out: if count == 0, return 0 */
    if (MEM32(ecx) == 0) {
        eax = 0;
        esp += 4; return;
    }

    /* Guard: if table_size (ecx+0x10) is 0, return 0 to avoid div-by-zero */
    uint32_t table_size_val = MEM32(ecx + 0x10);
    if (table_size_val == 0) {
        eax = 0;
        esp += 4; return;
    }

    /* Original logic: probe for non-null entry */
    eax = MEM32(ecx + 4);
    uint32_t table_ptr = MEM32(ecx + 0x14);

    if (MEM32(table_ptr + eax * 4) == 0) {
        /* Probe until we find a non-null slot */
        for (int i = 0; i < (int)table_size_val; i++) {
            eax = MEM32(ecx + 4);
            eax++;
            int32_t s_eax = (int32_t)eax;
            int32_t rem = s_eax % (int32_t)table_size_val;
            MEM32(ecx + 4) = (uint32_t)rem;
            if (MEM32(table_ptr + rem * 4) != 0) {
                eax = (uint32_t)rem;
                break;
            }
        }
    }

    /* If slot is still empty, return 0 */
    uint32_t idx = MEM32(ecx + 4);
    uint32_t entry = MEM32(table_ptr + idx * 4);
    if (entry == 0) {
        eax = 0;
        esp += 4; return;
    }

    /* Remove entry from table, add to free list */
    uint32_t next = MEM32(entry);
    MEM32(table_ptr + idx * 4) = next;
    uint32_t free_head = MEM32(ecx + 0x1C);
    MEM32(entry) = free_head;
    uint32_t count = MEM32(ecx);
    count--;
    MEM32(ecx + 0x1C) = entry;
    MEM32(ecx) = count;
    eax = MEM32(entry + 4);
    esp += 4; return;
}

/**
 * sub_001C66F0 - RW world linked list cleanup (STUB)
 *
 * Original: 0x001C66F0 - 0x001C67CE (222 bytes, 74 insns)
 *
 * Walks linked lists in the RW world rendering structure, clearing
 * entries and resetting float matrices. Contains an unbounded linked
 * list traversal that hangs on uninitialized/circular list data.
 *
 * Called with eax = pointer to RW world rendering context.
 * Also calls sub_001C1740 and sub_001BEFF0 (both already stubbed).
 *
 * Stubbed because RW world data isn't properly initialized.
 */
void sub_001C66F0(void)
{
    static int _1c66f0_count = 0;
    if (_1c66f0_count < 5)
        fprintf(stderr, "  [STUB] sub_001C66F0 (RW world cleanup) eax=0x%08X\n", eax);
    _1c66f0_count++;
    esp += 4; return; /* ret */
}

/**
 * sub_00022660 - Game state notification dispatch (RECURSION-GUARDED)
 *
 * Original: 0x00022660 - 0x000226C6 (102 bytes, 41 insns)
 * Category: game_vtable
 *
 * Dispatches state change notifications through vtable callbacks.
 * When RW world data isn't initialized, vtable pointers can form
 * circular chains causing infinite recursion → stack overflow.
 *
 * This override adds a recursion depth guard (max 32 levels).
 * CC: thiscall with 3 params on stack (ret 12)
 */
void sub_00022660(void)
{
    static int _depth = 0;
    static int _guard_count = 0;

    if (_depth >= 32) {
        if (_guard_count < 5)
            fprintf(stderr, "  [GUARD] sub_00022660 recursion depth %d, bailing\n", _depth);
        _guard_count++;
        esp += 16; return; /* ret 12 */
    }

    _depth++;

    /* Save params from stack */
    uint32_t param1 = MEM32(esp + 4);
    uint32_t param2 = MEM32(esp + 8);
    uint32_t param3 = MEM32(esp + 0xC);

    /* ecx = this (ebx in original), call sub_001B4170 */
    uint32_t saved_ebx = ebx;
    ebx = ecx;
    PUSH32(esp, param3);
    PUSH32(esp, param2);
    PUSH32(esp, param1);
    ecx = ebx;
    PUSH32(esp, 0); sub_001B4170();

    /* Call sub_001B4260 to hash-lookup the state */
    PUSH32(esp, 0x93D12267u);
    PUSH32(esp, 0x889D607Fu);
    PUSH32(esp, 0); sub_001B4260();

    uint32_t new_state = eax;
    if (MEM32(ebx + 4) != new_state) {
        /* Dispatch: call vtable[0x10](this, new_state, 0) */
        uint32_t vtable = MEM32(ebx);
        uint32_t method1 = MEM32(vtable + 0x10);
        recomp_func_t fn1 = recomp_lookup_manual(method1);
        if (!fn1) fn1 = recomp_lookup(method1);
        if (!fn1) fn1 = recomp_lookup_kernel(method1);
        if (fn1) {
            uint32_t _saved_esp = esp;
            PUSH32(esp, 0);
            PUSH32(esp, new_state);
            ecx = ebx;
            PUSH32(esp, 0);
            fn1();
        }

        /* Update state */
        uint32_t old_state = MEM32(ebx + 4);
        MEM32(ebx + 4) = new_state;

        /* If ebx+0x10 is non-null, update related pointers */
        uint32_t ptr = MEM32(ebx + 0x10);
        if (ptr != 0) {
            MEM32(ebx + 0x14) = MEM32(ptr + 0x20);
            if (new_state != 0) {
                MEM32(new_state + 0x10) = MEM32(ptr + 0x20);
            }
        }

        /* Dispatch: call vtable[0x14](this, old_state, 0) */
        vtable = MEM32(ebx);
        uint32_t method2 = MEM32(vtable + 0x14);
        recomp_func_t fn2 = recomp_lookup_manual(method2);
        if (!fn2) fn2 = recomp_lookup(method2);
        if (!fn2) fn2 = recomp_lookup_kernel(method2);
        if (fn2) {
            PUSH32(esp, 0);
            PUSH32(esp, old_state);
            ecx = ebx;
            PUSH32(esp, 0);
            fn2();
        }
    }

    ebx = saved_ebx;
    _depth--;
    esp += 16; return; /* ret 12 */
}

/**
 * sub_0003D9E0 - Game render orchestrator (STUB)
 *
 * Original: 0x0003D9E0 - 0x0003DA90 (176 bytes, 41 insns)
 * Category: game_render
 *
 * Called from the common exit path (loc_00016C42) of the main game tick
 * function sub_000165F0. Orchestrates a single frame of RenderWare rendering:
 *   1. sub_0002F330 - sets render state pointers based on edi (mode select)
 *   2. sub_0034D530 - D3D rendering (already stubbed separately)
 *   3. sub_00040660 - copies camera/matrix data from RW global tables
 *
 * All three callees chase pointers through uninitialized RenderWare structures
 * (vtable chains, D3D state objects), producing millions of garbage ICALLs to
 * addresses like 0x24000168, 0x23800068, etc. and leaking ESP massively
 * (24+ bytes per ICALL). This causes the game loop to hang after one iteration.
 *
 * Stub: skip all rendering operations. Our frame output comes from
 * game_frame_pump() called via sub_000110E0 instead.
 *
 * Implicit register params: esi = game object base (0x4D6170), edi = mode (0)
 * Calling convention: cdecl, caller pushes dummy ret addr
 */
void sub_0002F330(void);
void sub_00040660(void);

void sub_0003D9E0(void)
{
    /* Render orchestrator: called from loc_00016C42 with esi=0x4D6170, edi=0.
     * Original calls:
     *   1. sub_0002F330 (render state init, uses ecx=edi, eax=esi)
     *   2. sub_0034D530 (D3D viewport/camera setup) - SKIP, uses Xbox D3D
     *   3. sub_00040660 (copies camera/matrix data from RW tables)
     *
     * Try calling sub_0002F330 and sub_00040660 with guards.
     * Skip sub_0034D530 (touches Xbox D3D device). */
    static uint32_t call_count = 0;
    call_count++;

    esp = esp - 0x18;  /* Original stack frame */

    /* sub_0002F330: sets render state pointers (ecx=edi, eax=esi) */
    ecx = edi;
    eax = esi;
    PUSH32(esp, 0); sub_0002F330();

    if (call_count <= 5)
        fprintf(stderr, "  [RENDER] post-0002F330: 9B0=0x%X 9B8=0x%X 9A8=0x%X 3B0=0x%X\n",
                MEM32(esi + 0x9B0), MEM32(esi + 0x9B8), MEM32(esi + 0x9A8), MEM32(esi + 0x3B0));

    /* sub_0034D530: D3D8LTCG rendering pipeline.
     * Original code builds a render state struct on the stack from game object
     * fields, then passes a pointer to it. When edi==0 (menu path):
     *   esp+0 = MEM32(esi+0x9B0), esp+4 = MEM32(esi+0x9B8),
     *   esp+8 = MEM32(esi+0x9A8), ecx = &esp (struct pointer)
     * sub_0034D530 reads MEM32(ecx) to get the render list. */
    if (call_count <= 5) {
        /* Also check what sub_0002F330 sets up (it modifies render state) */
        uint32_t pre_9b0 = MEM32(esi + 0x9B0);
        uint32_t pre_9b8 = MEM32(esi + 0x9B8);
        fprintf(stderr, "  [RENDER] sub_0003D9E0 #%u: edi=%u esi=0x%X pre: 9B0=0x%X 9B8=0x%X 9A8=0x%X 3C0=0x%X 3B0=0x%X\n",
                call_count, edi, esi, pre_9b0, pre_9b8, MEM32(esi + 0x9A8), MEM32(esi + 0x3C0), MEM32(esi + 0x3B0));
    }

    if (TEST_Z(edi, edi)) {
        /* Menu render path: build render state struct like original code */
        uint32_t saved_esp = esp;
        eax = MEM32(esi + 0x9B0);
        uint32_t val_9a8 = MEM32(esi + 0x9A8);
        uint32_t val_9b8 = MEM32(esi + 0x9B8);
        uint32_t val_3c0 = MEM32(esi + 0x3C0);
        MEM32(esp)     = eax;           /* render state ptr */
        MEM32(esp + 4) = val_9b8;       /* render list count */
        MEM32(esp + 8) = val_9a8;       /* render list base */
        edx = val_9b8 + val_9b8;
        eax = val_3c0 - edx;
        MEMF(esp + 0x10) = 0.0f;
        float f_3b168c = MEMF(0x3B168C);
        ecx = esp;                       /* pointer to struct */
        PUSH32(esp, ecx);                /* push struct pointer as param */
        MEM32(esp + 0x10) = eax;
        MEMF(esp + 0x18) = f_3b168c;
        PUSH32(esp, 0); sub_0034D530();  /* call with render state */
        esp = saved_esp;                 /* restore stack */
    } else {
        PUSH32(esp, 0); sub_0034D530();  /* non-menu path: original param=0 */
    }

    /* sub_00040660: copies camera/matrix from RW tables */
    /* Only call if esi looks valid (should be 0x4D6170) */
    if (esi > 0x10000 && esi < 0x800000) {
        eax = MEM32(esi + 0x3B0);
        if (eax > 0x10000 && eax < 0x800000) {
            edx = MEM32(eax + 0x58);
            if (edx > 0x10000 && edx < 0x800000) {
                ecx = MEM32(edx + 0x68);
                MEM32(esi + 0x9A0) = ecx;
                edx = MEM32(eax + 0x58);
                ecx = MEM32(edx + 0x6C);
                MEM32(esi + 0x9A4) = ecx;
                MEM32(esi + 0x990) = 0;
                MEM32(esi + 0x994) = 0;
                edx = MEM32(eax + 0x78);
                MEM32(esi + 0x998) = edx;
                eax = MEM32(eax + 0x7C);
                edx = esi + 0x500;
                PUSH32(esp, esi);
                PUSH32(esp, 0); sub_00040660();

                if (call_count <= 3)
                    fprintf(stderr, "  [RENDER] sub_0003D9E0 #%u: esi=0x%08X, called 0002F330+00040660\n",
                            call_count, esi);
            }
        }
    }

    esp = esp + 0x18;
    esp += 4; return;
}

/**
 * sub_000636D0 - Car physics force computation (OVERRIDE)
 *
 * Original: 0x000636D0 - 0x00063A68 (920 bytes, 216 insns)
 * CC: cdecl, 0 params, returns int_or_void
 *
 * Computes throttle/steering forces from input accumulators and writes
 * them to the car's velocity vector. Also manages the boost state
 * machine and physics callbacks when boost is active.
 *
 * Override reason: Scale factors at 0x557870, 0x3B1C40, 0x5592C8,
 * 0x3B1C38 are zero/denormalized because the game's car/track init
 * doesn't fully initialize them in our recompilation. We fall back
 * to hardcoded scales when they're near-zero.
 *
 * Register input: esi = car object pointer (0x557880, set by caller)
 */
void sub_000636D0(void)
{
    uint32_t saved_ebx = ebx;
    uint32_t saved_edi = edi;
    float delta_time = MEMF(0x4AE1FC);
    uint8_t boost_end_flag = 0;

    /* Debug: log first call and periodically */
    {
        static int _dbg = 0;
        _dbg++;
        if (_dbg == 1 || (_dbg % 1000 == 0)) {
            fprintf(stderr, "  [PHY] #%d esi=0x%08X vel_ptr=0x%08X boost=%d\n",
                    _dbg, esi, MEM32(esi + 0x1B4), MEM8(0x4A4B90));
        }
    }

    /* ─── Part 0: Ensure physics body exists ─────────────────────── */
    /* The car's physics velocity pointer (esi+0x1B4) is NULL because
     * the game's physics world init path doesn't fully run in our
     * recompilation. Allocate a fake physics body in unused Xbox memory
     * so forces have somewhere to write. Address 0x5FFF00 is in free
     * BSS space past the image end (~0x5A4000). */
    {
        uint32_t vel_ptr_check = MEM32(esi + 0x1B4);
        if (vel_ptr_check == 0 || vel_ptr_check > 0x3FFFFFF) {
            static int _init_once = 0;
            if (!_init_once) {
                _init_once = 1;
                /* Zero out fake physics body (32 bytes) */
                MEM32(0x5FFF00 + 0) = 0;
                MEM32(0x5FFF00 + 4) = 0;
                MEM32(0x5FFF00 + 8) = 0;  /* vel.x */
                MEM32(0x5FFF00 + 0xC) = 0; /* vel.y */
                MEM32(0x5FFF00 + 0x10) = 0;
                MEM32(0x5FFF00 + 0x14) = 0;
                MEM32(0x5FFF00 + 0x18) = 0;
                MEM32(0x5FFF00 + 0x1C) = 0;
                fprintf(stderr,
                    "  [PHY] Allocated fake physics body at 0x5FFF00 "
                    "(old vel_ptr=0x%08X)\n", vel_ptr_check);
            }
            MEM32(esi + 0x1B4) = 0x5FFF00;
        }
    }

    /* ─── Part 1: Force computation (car model) ──────────────────── */
    /* Fake physics body layout at 0x5FFF00:
     *   +0x08: forward acceleration (set here, read by integrator)
     *   +0x0C: turn rate (set here, read by integrator)
     *   +0x10: pos_x (world, set by integrator)
     *   +0x14: pos_y (world, set by integrator)
     *   +0x18: heading (radians, 0=up/north, CW positive)
     *   +0x1C: speed (scalar forward speed)
     */
    {
        int32_t raw_thr = (int32_t)MEM32(0x4D652C)
                        - (int32_t)MEM32(0x4D6B24)
                        - (int32_t)MEM32(0x4D6B20);
        /* acceleration = raw_input * scale
         * Track mode: 10x acceleration for world-scale tracks
         *   W key (raw=1000): accel = 10.0 units/s^2 on track */
        float accel_scale = g_track_mode ? 0.010f : 0.001f;
        float accel_f = (float)raw_thr * accel_scale;

        int32_t raw_str = (int32_t)MEM32(0x4D6530)
                        - 2 * (int32_t)MEM32(0x4D6B28);
        /* turn_rate = raw_input * 0.003 radians/s (scaled by speed in integrator)
         *   A/D key (raw=1000): base turn rate = 3.0 rad/s */
        float turn_f = (float)raw_str * 0.003f;

        uint32_t vel_ptr = MEM32(esi + 0x1B4);
        MEMF(vel_ptr + 8) = accel_f;
        MEMF(vel_ptr + 0xC) = turn_f;

        /* Debug: log when non-zero force is applied */
        {
            static int _wr_dbg = 0;
            _wr_dbg++;
            if (_wr_dbg == 1 ||
                ((accel_f != 0.0f || turn_f != 0.0f) && _wr_dbg % 60 == 0)) {
                fprintf(stderr,
                    "  [PHY-WR] #%d accel=%.3f turn=%.3f spd=%.2f hdg=%.2f\n",
                    _wr_dbg, accel_f, turn_f, MEMF(vel_ptr + 0x1C), MEMF(vel_ptr + 0x18));
            }
        }
    }

    /* ─── Part 2: Boost state machine (only when boost active) ───── */
    if (MEM8(0x4A4B90) == 0)
        goto phy_epilogue;

    {
        uint32_t state = MEM32(esi + 0x1E4);
        switch (state) {
        case 0: { /* Ramp-up */
            float timer = MEMF(esi + 0x1DC);
            float threshold = MEMF(0x3B16E8);
            if (timer >= threshold) {
                MEM32(esi + 0x1E4) = 2;
                MEMF(esi + 0x1DC) = 0.0f;
                MEMF(esi + 0x1E0) = 0.0f;
            } else {
                float new_t = delta_time + timer;
                MEMF(esi + 0x1DC) = new_t;
                if (new_t >= threshold)
                    MEMF(esi + 0x1E0) = MEMF(0x3B168C);
                else
                    MEMF(esi + 0x1E0) = new_t * MEMF(0x557854);
            }
            break;
        }
        case 1: { /* Sustain */
            float timer = MEMF(esi + 0x1DC);
            float threshold = MEMF(0x557838);
            if (timer >= threshold) {
                MEM32(esi + 0x1E4) = 5;
            } else {
                float new_t = delta_time + timer;
                MEMF(esi + 0x1DC) = new_t;
                if (new_t >= threshold)
                    MEMF(esi + 0x1E0) = MEMF(0x3B168C);
                else
                    MEMF(esi + 0x1E0) = new_t * MEMF(0x557868);
            }
            break;
        }
        case 2: { /* Decay (variant A) */
            float timer = MEMF(esi + 0x1DC);
            if (timer <= 0.0f) {
                if (MEM8(esi + 0x19FF) != 0) {
                    MEMF(esi + 0x1DC) = MEMF(0x3B16E8);
                    MEMF(esi + 0x1E0) = MEMF(0x3B168C);
                    MEM32(esi + 0x1E4) = 1;
                    MEM8(esi + 0x19FF) = 0;
                } else {
                    MEMF(esi + 0x1DC) = 0.0f;
                    boost_end_flag = 1;
                }
            } else {
                float new_t = timer - delta_time;
                MEMF(esi + 0x1DC) = new_t;
                if (new_t <= 0.0f)
                    MEMF(esi + 0x1E0) = 0.0f;
                else
                    MEMF(esi + 0x1E0) = new_t * MEMF(0x557868);
            }
            break;
        }
        case 3: { /* Decay (variant B) */
            float timer = MEMF(esi + 0x1DC);
            if (timer <= 0.0f) {
                MEMF(esi + 0x1DC) = 0.0f;
                boost_end_flag = 1;
            } else {
                float new_t = timer - delta_time;
                MEMF(esi + 0x1DC) = new_t;
                if (new_t <= 0.0f)
                    MEMF(esi + 0x1E0) = 0.0f;
                else
                    MEMF(esi + 0x1E0) = new_t * MEMF(0x557854);
            }
            break;
        }
        default:
            break;
        }

        /* ─── Part 3: Post-boost callbacks ────────────────────────── */
        /* Check if boost ended and call sub_00063670 if conditions met */
        if (MEM8(esi + 0x19FD) == 0) {
            uint32_t mode = MEM32(0x4D4244);
            if ((mode == 0x17 || mode == 0x18 || mode == 1)
                && boost_end_flag) {
                eax = esi;
                PUSH32(esp, 0); sub_00063670();
            }
        }

        /* Compute callback args: edx=count, eax=param */
        edx = MEM32(esi + 0x1E8);
        if ((int32_t)edx >= 3)
            eax = 0;
        else
            eax = MEM32(esi + 0x1EC);

        /* Callback through 0x567174 vtable (ICALL_SAFE guards failure) */
        {
            uint32_t cb = MEM32(0x567174);
            if (cb != 0) {
                edi = MEM32(cb);
                uint32_t _icall_esp = g_esp;
                PUSH32(esp, eax);
                PUSH32(esp, edx);
                edx = MEM32(0x567178);
                PUSH32(esp, edx);
                PUSH32(esp, 5);
                PUSH32(esp, 0);
                RECOMP_ICALL_SAFE(MEM32(edi), _icall_esp);
            }
        }
    }

    /* loc_0006394D: Physics object callback */
    {
        uint32_t fptr = MEM32(esi + 0x1F0);
        if (fptr != 0) {
            /* ICALL through vtable at esi+0x1F0 */
            eax = MEM32(fptr);
            {
                uint32_t _icall_esp = g_esp;
                PUSH32(esp, 0);
                RECOMP_ICALL_SAFE(MEM32(eax), _icall_esp);
            }

            /* Walk linked list at esi+0x1D8 */
            edi = MEM32(esi + 0x1D8);
            if (edi != 0) {
                union { float f; uint32_t u; } dt_u;
                dt_u.f = delta_time;
                ebx = dt_u.u; /* delta_time as uint32_t for arg passing */
                while (edi != 0) {
                    if (MEM8(edi + 0xA) != 0) {
                        edx = MEM32(edi);
                        ecx = edi;
                        uint32_t _icall_esp = g_esp;
                        PUSH32(esp, ebx);
                        PUSH32(esp, 0);
                        RECOMP_ICALL_SAFE(MEM32(edx), _icall_esp);
                    }
                    edi = MEM32(edi + 4);
                }
            }
        } else {
            /* No physics object: check sound/feedback flag */
            if (MEM8(0x555D5A) != 0) {
                union { float f; uint32_t u; } dt_u;
                dt_u.f = delta_time;
                eax = dt_u.u;
                PUSH32(esp, eax);
                ecx = 0x555D50;
                PUSH32(esp, 0); sub_000C2DF0();
            }
        }
    }

    /* loc_00063996: Timer/counter update */
    edx = 0x4AE20C;
    PUSH32(esp, 0); sub_00017750();

    /* loc_000639A0: State transition logic */
    if (LO8(eax) != 0) goto phy_epilogue;
    if (MEM32(esi + 0x1F0) == 0x559F8C) goto phy_epilogue;
    if (MEM32(0x4D53B4) == 4) goto phy_epilogue;

    {
        /* Original checks 0x5A3759 for flags, 0x555D5A for sound state.
         * Note: original asm tests 0x5A3759 then overwrites al with 0x555D5A
         * but branches on the FLAGS from 0x5A3759 (not 0x555D5A). */
        uint8_t flag_5A = MEM8(0x5A3759);
        uint8_t flag_5D = MEM8(0x555D5A);

        if (flag_5D != 0) {
            /* loc_00063A1F: 0x555D5A is set */
            if (flag_5D != 0) goto phy_epilogue; /* always taken */
            MEM8(0x555D5A) = 1;
            MEM8(0x555D5B) = 0;
        } else {
            /* loc_000639CB */
            if (flag_5A == 0) {
                /* loc_000639CF: neither flag set */
                MEM8(0x555D5A) = 1;
                MEM8(0x555D5B) = 0;
            }
            /* loc_000639DD: check game mode */
            if (MEM32(0x4D537C) != 0xFFFFFFFFu) goto phy_epilogue;
            if (MEM32(esi + 0x1F0) != 0) goto phy_epilogue;

            /* loc_000639F6: compute and store controller connected flag */
            {
                int32_t idx = (int32_t)(int8_t)MEM8(0x4AED45);
                uint32_t slot_addr = (uint32_t)(idx * 0x188) + 0x4AE728;
                uint32_t slot = MEM32(slot_addr);
                edi = MEM32(slot + 0x11C);
                MEM32(0x4D5380) = (edi != 0xFFFFFFFFu) ? 1 : 0;
            }
        }
    }

phy_epilogue:
    /* loc_00063A31: Flag management and cleanup */
    if (MEM8(esi + 0x19FD) != 0) {
        if (MEM32(esi + 0x1F0) == 0) {
            /* Call sub_000146E0 with notification constants */
            edi = 0;
            PUSH32(esp, 0x94413FA7u);
            PUSH32(esp, 0x37AAA797);
            PUSH32(esp, 0x567170);
            PUSH32(esp, 0); sub_000146E0();
        }
        MEM8(esi + 0x19FD) = 0;
    }

    /* Restore callee-saved registers */
    ebx = saved_ebx;
    edi = saved_edi;
    esp += 4; return; /* pop dummy return address */
}

/**
 * sub_00011240 - Resource load queue handler (OVERRIDE)
 *
 * Original: 0x00011240 - 0x000113E4 (420 bytes, 131 insns)
 * CC: stdcall, ret 20 (5 params)
 *
 * Original flow: queues a file load request, then in the game loop tick
 * (sub_000110E0) the RW stream reader (sub_001B33A0) would process it.
 * Since the RW stream reader hangs on NV2A, the old patch just set
 * completion_flag = 1 immediately without loading any data.
 *
 * This override actually loads the file from disk into Xbox heap memory,
 * then stores the pointer in the resource slot so the game's own code
 * can process the loaded data.
 *
 * NOTE: .rdata strings get corrupted at runtime because .rdata is not
 * write-protected (page boundary issue with .data). We read filenames
 * from the original XBE data (g_xbe_data) when name_va is in .rdata.
 *
 * Stack args (stdcall, 5 params, ret 20):
 *   arg1 [esp+4]:  queue_ptr  - resource queue object (this)
 *   arg2 [esp+8]:  name_va    - Xbox VA of file path string
 *   arg3 [esp+12]: flag_va    - Xbox VA of completion flag byte
 *   arg4 [esp+16]: resource   - resource slot data pointer (Xbox VA)
 *   arg5 [esp+20]: param      - parameter (version/tag)
 */
void sub_00011240(void)
{
    /* Read stack args */
    uint32_t queue_va    = MEM32(esp + 4);
    uint32_t name_va     = MEM32(esp + 8);
    uint32_t flag_va     = MEM32(esp + 12);
    uint32_t resource_va = MEM32(esp + 16);
    uint32_t param       = MEM32(esp + 20);

    /* Read the file name string.
     * .rdata strings get corrupted at runtime, so when name_va is in
     * the .rdata VA range, read from the original XBE data instead. */
    extern void *g_xbe_data;
    extern size_t g_xbe_size;
    #define RDATA_VA_START  0x0036B7C0
    #define RDATA_VA_END    0x003B2354
    #define RDATA_FILE_OFF  0x0035C000

    const char *name_ptr;
    if (name_va >= RDATA_VA_START && name_va < RDATA_VA_END && g_xbe_data) {
        /* Read from original XBE data (uncorrupted) */
        uint32_t file_off = (name_va - RDATA_VA_START) + RDATA_FILE_OFF;
        name_ptr = (const char *)((uint8_t *)g_xbe_data + file_off);
    } else {
        /* Read from Xbox memory (runtime, may be constructed on stack/heap) */
        name_ptr = (const char *)XBOX_PTR(name_va);
    }

    char name_buf[128];
    {
        int i;
        for (i = 0; i < 127 && name_ptr[i]; i++)
            name_buf[i] = name_ptr[i];
        name_buf[i] = '\0';
    }

    fprintf(stderr, "  [LOAD] sub_00011240: name_va=0x%08X flag=0x%08X res=0x%08X param=0x%08X\n",
            name_va, flag_va, resource_va, param);
    fprintf(stderr, "  [LOAD]   file: '%s'\n", name_buf);

    /* Detect corrupted filenames for static.dat / streamed.dat.
     * The render list builder constructs the path on the stack but gen code
     * corrupts the string. Detect by: the name contains "static.dat" but
     * the first char is NOT a valid path char (alphanumeric, d:, /, \). */

    /* Try to translate the Xbox path and load the file.
     * Xbox paths like "d:\tracks\..." → "Burnout 3 Takedown\tracks\..."
     * But the string might also be a relative path within the game dir. */
    {
        char win_path[512];
        FILE *fp = NULL;

        /* Try direct path first (relative to game dir) */
        snprintf(win_path, sizeof(win_path), "Burnout 3 Takedown\\%s", name_buf);
        fp = fopen(win_path, "rb");

        if (!fp) {
            /* Try with d:\ prefix stripped */
            const char *p = name_buf;
            if ((p[0] == 'd' || p[0] == 'D') && p[1] == ':' && (p[2] == '\\' || p[2] == '/'))
                p += 3;
            else if (p[0] == '\\')
                p++;
            snprintf(win_path, sizeof(win_path), "Burnout 3 Takedown\\%s", p);
            fp = fopen(win_path, "rb");
        }

        if (!fp) {
            /* Try the name as-is */
            fp = fopen(name_buf, "rb");
        }

        /* Fallback for corrupted filenames containing "static.dat" or "streamed.dat":
         * use the currently loaded track directory to construct the correct path.
         * DISABLED: loading static.dat into the render list resource causes the
         * relocated data to trigger spin loops in gen code during boot init.
         * TODO: investigate which gen code function spins on the relocated data. */
        if (!fp && (strstr(name_buf, "static.dat") || strstr(name_buf, "streamed.dat"))) {
            extern const char *rw_get_track_dir(void);
            const char *tdir = rw_get_track_dir();
            if (tdir && tdir[0]) {
                const char *dat_name = strstr(name_buf, "static.dat") ? "static.dat" : "streamed.dat";
                snprintf(win_path, sizeof(win_path), "%s/%s", tdir, dat_name);
                fp = fopen(win_path, "rb");
                if (fp)
                    fprintf(stderr, "  [LOAD] Fallback: opened '%s' via track dir\n", win_path);
            }
        }

        if (fp) {
            /* Get file size */
            fseek(fp, 0, SEEK_END);
            long fsize = ftell(fp);
            fseek(fp, 0, SEEK_SET);

            fprintf(stderr, "  [LOAD] Opened '%s' (%ld bytes)\n", win_path, fsize);

            if (fsize > 0 && fsize < 64 * 1024 * 1024) {
                /* The original async loader reads file data directly into the
                 * resource buffer (resource_va). For simple queue entries (like
                 * vlist.bin, tlist.bin), resource_va IS the destination buffer
                 * and param is its size. For complex entries (like Global.txd),
                 * resource_va is a pre-allocated structure from sub_00018BB0.
                 *
                 * In both cases: read data directly into resource_va.
                 * For safety, if param looks like a reasonable buffer size
                 * (0 < param <= 64MB), limit the read to that. */
                size_t max_read = (size_t)fsize;
                if (param > 0 && param <= 64 * 1024 * 1024 && param < (uint32_t)fsize) {
                    max_read = param;
                    fprintf(stderr, "  [LOAD] Clamping read to param=%u bytes\n", param);
                }

                if (resource_va > 0x100 && resource_va < 0x4000000) {
                    uint8_t *dest = (uint8_t *)XBOX_PTR(resource_va);
                    size_t nread = fread(dest, 1, max_read, fp);
                    fprintf(stderr, "  [LOAD] Read %zu bytes directly into Xbox VA 0x%08X\n",
                            nread, resource_va);
                } else {
                    /* Fallback: allocate heap buffer if resource_va is invalid */
                    extern uint32_t xbox_HeapAlloc(uint32_t size, uint32_t alignment);
                    uint32_t data_va = xbox_HeapAlloc((uint32_t)fsize + 16, 16);
                    if (data_va != 0) {
                        uint8_t *data_ptr = (uint8_t *)XBOX_PTR(data_va);
                        size_t nread = fread(data_ptr, 1, (size_t)fsize, fp);
                        fprintf(stderr, "  [LOAD] Read %zu bytes into heap Xbox VA 0x%08X\n",
                                nread, data_va);
                    } else {
                        fprintf(stderr, "  [LOAD] FAILED: xbox_HeapAlloc(%ld) returned 0\n", fsize);
                    }
                }
            }
            fclose(fp);
        } else {
            fprintf(stderr, "  [LOAD] FAILED: could not open '%s' (tried Xbox + relative)\n",
                    name_buf);
        }
    }

    /* Set completion flag = 1 so the game knows the resource is ready */
    if (flag_va > 0x100 && flag_va < 0x4000000) {
        MEM8(flag_va) = 1;
    }

    /* Still need to maintain the queue state.
     * Run the original queue index management logic. */
    {
        uint32_t tail = MEM32(queue_va + 0x78C);
        /* Entry stride = idx * 80 (0x50) */
        uint32_t entry_base = queue_va + tail * 80;

        /* Copy filename into queue entry */
        {
            int i;
            for (i = 0; i < 63 && name_buf[i]; i++)
                MEM8(entry_base + (uint32_t)i) = (uint8_t)name_buf[i];
            MEM8(entry_base + (uint32_t)i) = 0;
        }

        /* Store fields */
        MEM32(entry_base + 0x40) = flag_va;
        MEM32(entry_base + 0x44) = resource_va;
        MEM32(entry_base + 0x48) = param;

        /* Version counter */
        uint32_t ver = MEM32(queue_va + 0x790);
        MEM32(entry_base + 0x4C) = ver;
        ver++;
        if (ver == 0) ver = 1;
        MEM32(queue_va + 0x790) = ver;

        /* Advance tail index (circular, 24 entries) */
        tail++;
        if (tail >= 0x18) tail = 0;
        MEM32(queue_va + 0x78C) = tail;
    }

    /* Call sub_000110E0 to process the tick (like original code does) */
    edi = queue_va;
    PUSH32(esp, 0); sub_000110E0();

    eax = MEM32(queue_va + 0x790); /* return version like original */
    esp += 24; return; /* ret 20 */
}

/* sub_00157680 removed from manual overrides - gen code used directly now
 * that sub_00159710 implements full pointer relocation for PrgData.bin */

/* =================================================================
 * RW Software Rasterizer Stubs
 * These are overlapping entry points into the 162KB RW software
 * pixel-processing code (0x002CC30D-0x002F3F34). Not needed with
 * our D3D11 hardware rendering backend.
 * ================================================================= */
void sub_002CC30D(void) { /* RW software rasterizer - stubbed */ }
void sub_002CC851(void) { /* RW software rasterizer - stubbed */ }
void sub_002CC935(void) { /* RW software rasterizer - stubbed */ }
void sub_002CC951(void) { /* RW software rasterizer - stubbed */ }
void sub_002CCB1C(void) { /* RW software rasterizer - stubbed */ }

/* =================================================================
 * sub_000171A0 - Frontend object render dispatch (TRACED)
 *
 * Original: reads frontend object from game_base+0x2E1D0,
 * gets vtable, calls vtable[3] (render method).
 * Override: same logic but with diagnostic tracing to see
 * what render method is being called and whether it succeeds.
 * ================================================================= */
void sub_000171A0(void)
{
    static uint32_t call_count = 0;
    uint32_t ebp_local;  /* ebp is local, not a global register */
    uint32_t game_base, frontend_obj, vtable, render_method;
    float xmm0, xmm1, xmm2, xmm3;

    call_count++;
    if (call_count <= 5)
        fprintf(stderr, "  [FRONTEND] sub_000171A0 ENTERED #%u esp=0x%08X\n", call_count, esp);

    /* Standard EBP-based prologue (ebp is local) */
    PUSH32(esp, g_seh_ebp);
    ebp_local = esp;
    esp = esp - 0x14;

    /* Floating point delta tracking (original code) */
    xmm0 = MEMF(0x4D6198);
    xmm2 = MEMF(0x4D61A0);
    xmm3 = MEMF(0x4D61A4);
    xmm1 = MEMF(0x4D619C);

    if (xmm0 != xmm2 || xmm1 != xmm3) {
        float dx = xmm0 - xmm2;
        float dy = xmm1 - xmm3;
        MEMF(0x4D618C) = MEMF(0x4D618C) + dx;
        MEMF(0x4D6190) = MEMF(0x4D6190) + dy;
        MEM32(0x4D61A0) = MEM32(0x4D6198);
        MEM32(0x4D61A4) = MEM32(0x4D619C);
    }

    /* Get frontend object and call its render method.
     * ebp_local + 8 = first param (game_base, pushed by caller) */
    game_base = MEM32(ebp_local + 8);
    frontend_obj = MEM32(game_base + 0x2E1D0);

    if (is_valid_game_ptr(frontend_obj)) {
        /* FIX: Restore vtable pointer and entries if corrupted.
         * xemu shows MEM32(0x4D4008) should be 0x003A9E7C (vtable in .rdata).
         * RW linked-list code corrupts vtable ptr and entries [3],[4] to native ptrs.
         * Original vtable (from XBE .rdata before corruption):
         *   [0]=0x14760 [1]=0x14860 [2]=0x14C80 [3]=0x14D20
         *   [4]=0x14D80 [5]=0x23C10 [6]=0x17740 [7]=0x15500 */
        vtable = MEM32(frontend_obj);
        if (frontend_obj == 0x4D4008) {
            if (vtable != 0x003A9E7C) {
                if (call_count <= 3)
                    fprintf(stderr, "  [FRONTEND] #%u: FIXING vtable ptr 0x%08X → 0x003A9E7C\n",
                            call_count, vtable);
                MEM32(frontend_obj) = 0x003A9E7C;
                vtable = 0x003A9E7C;
            }
            /* Restore corrupted vtable entries [3] and [4] */
            if (MEM32(vtable + 0xC) != 0x00014D20) {
                if (call_count <= 3)
                    fprintf(stderr, "  [FRONTEND] #%u: FIXING vt[3] 0x%08X → 0x00014D20\n",
                            call_count, MEM32(vtable + 0xC));
                MEM32(vtable + 0xC)  = 0x00014D20;
                MEM32(vtable + 0x10) = 0x00014D80;
            }
        }

        if (is_valid_game_ptr(vtable)) {
            render_method = MEM32(vtable + 0xC);

            if (call_count <= 10 || (call_count % 5000) == 0) {
                fprintf(stderr, "  [FRONTEND] #%u: fe_obj=0x%08X vtable=0x%06X\n",
                        call_count, frontend_obj, vtable);
                fprintf(stderr, "    vt[0..7]=0x%08X 0x%08X 0x%08X 0x%08X 0x%08X 0x%08X 0x%08X 0x%08X\n",
                        MEM32(vtable), MEM32(vtable+4), MEM32(vtable+8),
                        render_method, MEM32(vtable+0x10), MEM32(vtable+0x14),
                        MEM32(vtable+0x18), MEM32(vtable+0x1C));
            }

            if (render_method > 0x10000 && render_method < 0x400000) {
                /* Valid Xbox VA - make the vtable call */
                { uint32_t _icall_esp = g_esp;
                PUSH32(esp, 0); RECOMP_ICALL_SAFE(render_method, _icall_esp);
                }
            } else if (call_count <= 10) {
                fprintf(stderr, "  [FRONTEND] #%u: render_method=0x%08X (INVALID)\n",
                        call_count, render_method);
            }
        } else {
            if (call_count <= 10)
                fprintf(stderr, "  [FRONTEND] #%u: fe_obj=0x%08X vtable=0x%08X (INVALID)\n",
                        call_count, frontend_obj, vtable);
        }
    } else {
        if (call_count <= 10)
            fprintf(stderr, "  [FRONTEND] #%u: base=0x%08X fe_obj=0x%08X (INVALID)\n",
                    call_count, game_base, frontend_obj);
    }

    /* Epilogue: ret 4 (stdcall) */
    esp = ebp_local;
    POP32(esp, g_seh_ebp);
    esp += 8; return;
}

/* =================================================================
 * sub_001AA100 - Frontend initialization state machine (OVERRIDE)
 *
 * Original: 0x001AA100 - 0x001AA6A8 (1448 bytes, 346 insns)
 * Category: game_network
 * CC: stdcall, 1 param (game object base, usually 0x60EA00)
 *
 * Multi-phase state machine stored at ebp+0x144384.
 * Phases 1-6 initialize frontend screen data structures.
 * Phase 6 checks B790/B794 (screen definition pointers).
 * Phases 7-16 build render lists when screen definitions exist.
 * Phase 0x13+ = per-frame body (ready to render).
 *
 * Previous override: skipped all phases → B790/B794 never populated
 * → render list empty → no menus rendered.
 *
 * New approach: Run phases with safeguards. Initialize phase to 1
 * if garbage, let it progress naturally. Skip dangerous ICALLs
 * that crash on uninitialized vtables.
 * ================================================================= */
void sub_001AA100(void)
{
    static uint32_t call_count = 0;
    uint32_t bp; /* game object base (ebp in original) */

    /* stdcall: 1 param on stack */
    PUSH32(esp, ebx);
    PUSH32(esp, g_seh_ebp);
    bp = MEM32(esp + 0xC);
    PUSH32(esp, esi);
    PUSH32(esp, edi);

    call_count++;

    uint32_t phase = MEM32(bp + 0x144384);

    if (call_count <= 50 || (call_count % 1000) == 0) {
        fprintf(stderr, "  [1AA100] #%u bp=0x%08X phase=%u B790=0x%08X B794=0x%08X B79C=%u\n",
                call_count, bp, phase,
                MEM32(bp + 0x12B790), MEM32(bp + 0x12B794),
                MEM32(bp + 0x12B79C));
    }

    /* Fix garbage phase on first call */
    if (phase > 0x100) {
        fprintf(stderr, "  [1AA100] Fixing garbage phase 0x%08X → 1\n", phase);
        MEM32(bp + 0x144384) = 1;
        phase = 1;
    }

    /* Early exit flag check (original code) */
    if (MEM8(0x55927C) != 0) {
        goto ret0;
    }

    /* ---- Phase state machine ---- */
    switch (phase) {
    case 0:
        /* Not initialized yet — set to 1 */
        MEM32(bp + 0x144384) = 1;
        goto ret0;

    case 1:
        /* Phase 1: vtable calls on 0x4CFB20 and 0x4D0770.
         * These are RW camera/scene objects. Skip the ICALLs
         * (they may crash on uninitialized vtables) and just
         * call sub_001B5A80 + advance. */
        fprintf(stderr, "  [1AA100] Phase 1: skip ICALLs, call sub_001B5A80\n");
        ecx = bp;
        PUSH32(esp, 0); sub_001B5A80();
        MEM32(bp + 0x12B7A0) = 0;
        MEM32(bp + 0x144384) = 3;
        goto ret0;

    case 2:
        /* Fallthrough to phase 3 */
        MEM32(bp + 0x144384) = 3;
        /* fall through */

    case 3: {
        /* Phase 3: call sub_0018B250 (check if something is ready) */
        edi = bp + 0x1265C0;
        PUSH32(esp, 0); sub_0018B250();
        if ((eax & 0xFF) == 0) {
            if (call_count <= 50)
                fprintf(stderr, "  [1AA100] Phase 3: sub_0018B250 returned 0, waiting...\n");
            goto ret0;
        }
        fprintf(stderr, "  [1AA100] Phase 3 → 4: sub_0018B250 ready\n");
        MEM32(bp + 0x144384) = 4;
        /* fall through */
    }

    case 4: {
        /* Phase 4: call sub_0013EA20 (audio/sound bank init).
         * Audio system is now initialized via our sub_00135040 override.
         * sub_0013EA20 manages the sound bank state machine — it needs
         * multiple calls to progress through its internal states.
         * Timeout after 15 tries (it may not fully complete without
         * RW pipeline nodes, but it sets enough state to proceed). */
        static uint32_t phase4_tries = 0;
        phase4_tries++;
        PUSH32(esp, 0x40E120);
        PUSH32(esp, 0); sub_0013EA20();
        if ((eax & 0xFF) == 0) {
            if (phase4_tries <= 15) {
                if (phase4_tries <= 3 || (phase4_tries % 5) == 0)
                    fprintf(stderr, "  [1AA100] Phase 4: sub_0013EA20 returned 0 (try %u)\n",
                            phase4_tries);
                if (phase4_tries >= 15) {
                    fprintf(stderr, "  [1AA100] Phase 4: audio init timeout after %u tries, advancing\n",
                            phase4_tries);
                    MEM32(bp + 0x144384) = 5;
                    goto ret0;
                }
                goto ret0;
            }
        } else {
            fprintf(stderr, "  [1AA100] Phase 4 → 5: audio ready (try %u)\n", phase4_tries);
        }
        MEM32(bp + 0x144384) = 5;
        /* fall through */
    }

    case 5: {
        /* Phase 5: iterate scene descriptors (B7A8 array).
         * Call vtable[0] on each descriptor object. */
        uint32_t desc_count = MEM32(bp + 0x12B79C);
        uint32_t desc_idx = MEM32(bp + 0x12B7A0);
        if (desc_count > 100) {
            /* Garbage count (often 0x45BAD0 = same as B790 pointer) — skip */
            fprintf(stderr, "  [1AA100] Phase 5: garbage desc_count=0x%X, zeroing\n", desc_count);
            MEM32(bp + 0x12B79C) = 0;
            desc_count = 0;
        }
        /* If no descriptors, advance to phase 6 */
        if (desc_idx >= desc_count || desc_count == 0) {
            fprintf(stderr, "  [1AA100] Phase 5 → 6: descriptors done (%u/%u)\n",
                    desc_idx, desc_count);
            MEM32(bp + 0x144384) = 6;
            goto phase6;
        }
        /* Try calling vtable[0] on each descriptor */
        while (desc_idx < desc_count) {
            uint32_t desc = MEM32(bp + desc_idx * 4 + 0x12B7A8);
            if (desc == 0 || desc > 0x4000000) {
                desc_idx++;
                continue;
            }
            uint32_t vt = MEM32(desc);
            if (vt == 0 || vt > 0x4000000) {
                desc_idx++;
                continue;
            }
            uint32_t method = MEM32(vt);
            if (method == 0 || method > 0x4000000) {
                desc_idx++;
                continue;
            }
            fprintf(stderr, "  [1AA100] Phase 5: desc[%u]=0x%08X vt=0x%08X [0]=0x%08X\n",
                    desc_idx, desc, vt, method);
            ecx = desc;
            { uint32_t _icall_esp = g_esp;
            PUSH32(esp, 0); RECOMP_ICALL_SAFE(method, _icall_esp);
            }
            if ((eax & 0xFF) == 0) {
                goto ret0;
            }
            desc_idx++;
            MEM32(bp + 0x12B7A0) = desc_idx;
        }
        MEM32(bp + 0x144384) = 6;
        /* fall through */
    }

    case 6:
    phase6: {
        /* Phase 6: check B790|B794 (screen definition pointers).
         * If populated (non-zero), proceed to build render lists.
         * If zero AND camera is gameplay (0x4D4290), go to per-frame body.
         * If zero AND camera is menu (0x4D4008), set up default scene
         * descriptors from within the base object. */
        uint32_t b790 = MEM32(bp + 0x12B790);
        uint32_t b794 = MEM32(bp + 0x12B794);
        fprintf(stderr, "  [1AA100] Phase 6: B790=0x%08X B794=0x%08X cam=0x%08X\n",
                b790, b794, MEM32(0x4D5370));

        if ((b790 | b794) != 0) {
            /* Screen definitions exist → proceed to phase 7 */
            fprintf(stderr, "  [1AA100] Phase 6: screen defs found → phase 7\n");
            MEM32(bp + 0x144384) = 7;
            goto phase7;
        }

        if (MEM32(0x4D5370) == 0x4D4290) {
            /* Gameplay camera → skip to per-frame body */
            fprintf(stderr, "  [1AA100] Phase 6: gameplay camera → phase 0x13\n");
            MEM32(bp + 0x144384) = 0x13;
            goto phase_body;
        }

        /* Menu camera — set up default scene descriptors from base object */
        uint32_t sd0 = bp + 0x130790;
        uint32_t sd1 = bp + 0x132C00;
        MEM32(bp + 0x12B7A8) = sd0;
        MEM32(bp + 0x12B7AC) = sd1;
        MEM8(sd0 + 0x19BC) = 0;
        MEM8(sd1 + 0x19BC) = 1;
        MEM32(bp + 0x12B79C) = 2; /* 2 scene descriptors */
        fprintf(stderr, "  [1AA100] Phase 6: set default scene descs: sd0=0x%08X sd1=0x%08X\n",
                sd0, sd1);
        MEM32(bp + 0x144384) = 0x13;
        goto phase_body;
    }

    case 7:
    phase7: {
        /* Phase 7: sub_0018E820 + sub_001888F0 */
        esi = bp + 0x126B50;
        PUSH32(esp, 0); sub_0018E820();

        if (MEM8(bp + 0x144380) != 0) {
            MEM8(bp + 0x12DFA9) = 1;
            MEM8(bp + 0x130789) = 1;
        }
        MEM32(bp + 0x144384) = 8;

        ebx = 0x60E040;
        PUSH32(esp, 0); sub_001888F0();
        if ((eax & 0xFF) == 0) {
            fprintf(stderr, "  [1AA100] Phase 7: sub_001888F0 returned 0, waiting...\n");
            goto ret0;
        }
        fprintf(stderr, "  [1AA100] Phase 7 → 8: ready\n");
        MEM32(bp + 0x144384) = 9; /* skip phase 8 (render entry init) for now */
        /* fall through to phase 9 */
    }

    case 8:
        /* Phase 8: render entry initialization — just advance */
        MEM32(bp + 0x144384) = 9;
        /* fall through */

    case 9: {
        /* Phase 9: sub_0019AE10 — BUILD THE RENDER LIST.
         * sub_0019AE10 has its own internal state machine at render_base+0x08.
         * State 0 = uninitialized (returns 0 immediately).
         * State 1 = begin loading screen resources.
         * Must set to 1 to kick off initialization.
         *
         * TIMEOUT: If sub_0019AE10 doesn't complete after 30 calls,
         * skip to phase 0x13 anyway. Track resources (sub_00062BD0)
         * are stubbed, so the render list builder can't finish.
         * We render track geometry via PB injection instead. */
        static uint32_t phase9_tries = 0;
        phase9_tries++;

        uint32_t render_base = bp + 0x12ADB0;
        uint32_t rlist_state = MEM32(render_base + 0x08);
        if (rlist_state == 0 || rlist_state > 0x18) {
            MEM32(render_base + 0x08) = 1;
            MEM8(render_base) = 0;
            MEM32(render_base + 0x04) = 0;
            MEM32(render_base + 0x0C) = 0;
        }
        if (call_count <= 50 || (call_count % 1000) == 0) {
            fprintf(stderr, "  [1AA100] Phase 9: sub_0019AE10(0x%08X) rlist_state=%u try=%u\n",
                    render_base, MEM32(render_base + 0x08), phase9_tries);
        }

        /* Try sub_0019AE10 */
        PUSH32(esp, render_base);
        PUSH32(esp, 0); sub_0019AE10();

        if ((eax & 0xFF) != 0) {
            phase9_tries = 0;
            MEM32(bp + 0x144384) = 0x13;
            fprintf(stderr, "  [1AA100] Phase 9 → 0x13: render list built!\n");
            goto phase_body;
        }

        /* Timeout: skip to gameplay after 30 attempts */
        if (phase9_tries >= 30) {
            phase9_tries = 0;
            MEM32(bp + 0x144384) = 0x13;
            /* Mark render list as "complete" so state machine proceeds */
            MEM32(render_base + 0x08) = 0x17;
            /* Signal race init completion to fe_menu so it stops forcing state=4 */
            {
                extern int g_race_init_done;
                g_race_init_done = 1;
                fprintf(stderr, "  [1AA100] Phase 9 TIMEOUT → 0x13 (gameplay). "
                        "g_race_init_done=1\n");
            }
            goto phase_body;
        }

        goto ret0;
    }

    case 0x0A: case 0x0B: case 0x0C: case 0x0D:
    case 0x0E: case 0x0F: case 0x10: case 0x11:
    case 0x12:
        /* Phases 10-18: intermediate phases we skip for now */
        MEM32(bp + 0x144384) = 0x13;
        goto phase_body;

    case 0x13:
    default:
    phase_body:
        /* Per-frame body: the frontend is initialized, return 1
         * to allow the outer state machine to transition. */
        if (call_count <= 50 || (call_count % 1000) == 0) {
            fprintf(stderr, "  [1AA100] Phase 0x13 (body): return 1\n");
        }
        /* Return 1: initialization complete */
        POP32(esp, edi);
        POP32(esp, esi);
        POP32(esp, g_seh_ebp);
        SET_LO8(eax, 1);
        POP32(esp, ebx);
        esp += 8; return;
    }

ret0:
    POP32(esp, edi);
    POP32(esp, esi);
    POP32(esp, g_seh_ebp);
    SET_LO8(eax, 0);
    POP32(esp, ebx);
    esp += 8; return; /* ret 4 (stdcall) */
}

/* =================================================================
 * sub_00014D20 - Frontend render method (OVERRIDE)
 *
 * Original: 0x00014D20 - 0x00014D7B (91 bytes, 25 insns)
 * Category: game_render, CC: cdecl, 0 params
 *
 * Called from sub_000171A0 via vtable[3] of frontend object 0x4D4008.
 * Original code:
 *   1. Calls sub_0034C2E0(0,0,0xF3,0,1.0f,0) - D3D Clear (83K function!)
 *   2. Checks MEM32(0x557A70) against screen object addresses
 *   3. If match, calls sub_001AE6F0(0x60EA00, byte_from_0x55609E)
 *
 * Override: replace sub_0034C2E0 with a proper D3D clear, log the
 * condition value, and call sub_001AE6F0 to enable menu rendering.
 * ================================================================= */
void sub_00014D20(void)
{
    static uint32_t call_count = 0;
    uint32_t screen_obj;

    call_count++;

    /* Step 0: Initialize frontend state.
     * Do NOT force MEM32(0x557A70) to non-zero - that causes sub_001C67D0
     * to be called in the state 5 loop (recomp_0000.c line 10304), which
     * freezes on native heap pointers. Keep 0x557A70=0 so that code path
     * is skipped. We call sub_001AE6F0 unconditionally below anyway. */
    if (call_count == 1) {
        SMEM8(0x55609E) = -1;
        fprintf(stderr, "  [FE-INIT] phase=-1, screen_obj left at 0 (avoids sub_001C67D0)\n");
    }

    /* Step 1: Clear the screen (replaces 83K sub_0034C2E0 call).
     * The gen code pushes (0, 0, 0xF3, 0, 1.0f, 0) = Clear(0, NULL, 0xF3, 0x000000, 1.0f, 0).
     * We skip the massive function and clear directly via our D3D8 layer. */
    {
        IDirect3DDevice8 *dev = d3d8_GetDevice();
        if (dev) {
            dev->lpVtbl->Clear(dev, 0, NULL, 0x07, 0x00000000, 1.0f, 0);
        }
    }

    /* Step 2: Check condition - which screen object is active? */
    screen_obj = MEM32(0x557A70);
    MEM8(0x4D6B30) = 1;  /* original code sets this flag */

    /* NOTE: screen_obj should be one of 0x559648/0x55A408/0x55CB60/0x55A118
     * but the frontend init code (sub_0006FC90 chain) that populates 0x557A70
     * never ran because state 6 (case 5 in switch) is never reached.
     * State progression: 1→7→4→5 skips state 6 which calls sub_001AA990.
     * Without 0x557A70, sub_001AE6F0 is never called and no im2d menu draws happen. */
    if (call_count <= 10 || (call_count % 5000) == 0) {
        fprintf(stderr, "  [FE-RENDER] sub_00014D20 #%u: screen_obj=0x%08X phase=%d\n",
                call_count, screen_obj, (int32_t)SMEM8(0x55609E));
        /* Dump nearby state to help debug the init chain */
        if (call_count <= 3) {
            fprintf(stderr, "    obj+0x1B8=0x%08X [567174]=0x%08X [567178]=0x%08X\n",
                    MEM32(0x4D4008 + 0x1B8), MEM32(0x567174), MEM32(0x567178));
            fprintf(stderr, "    vt@3A9E7C: [0..7]=0x%X 0x%X 0x%X 0x%X 0x%X 0x%X 0x%X 0x%X\n",
                    MEM32(0x3A9E7C+0), MEM32(0x3A9E7C+4), MEM32(0x3A9E7C+8),
                    MEM32(0x3A9E7C+0xC), MEM32(0x3A9E7C+0x10), MEM32(0x3A9E7C+0x14),
                    MEM32(0x3A9E7C+0x18), MEM32(0x3A9E7C+0x1C));
        }
    }

    /* Step 3: Always render menu via our override (gen code is stubbed) */
    {
        int32_t phase = (int32_t)SMEM8(0x55609E);
        if (call_count <= 20 || (call_count % 5000) == 0)
            fprintf(stderr, "  [FE-RENDER] #%u: Calling sub_001AE6F0(0x60EA00, %d)\n",
                    call_count, phase);
        PUSH32(esp, (uint32_t)phase);
        PUSH32(esp, 0x60EA00);
        PUSH32(esp, 0); sub_001AE6F0();
    }

    /* Step 4: Present frame. The normal RW pipeline calls Present through
     * sub_001D9420 → sub_001DDAF0 → sub_00351090. But since we're rendering
     * directly via im2d, we present here. */
    d3d8_PresentFrame();

    /* ret (cdecl, caller cleanup) */
    esp += 4; return;
}

/* =================================================================
 * sub_001AE6F0 - Frontend Render Dispatch (OVERRIDE)
 *
 * Original: 0x001AE6F0 - 0x001AEAA0 (944 bytes, 213 insns)
 * CC: stdcall, 2 params (base_obj, phase), returns void
 *
 * Original call sequence:
 *   sub_001891F0 → sub_0003FEE0 → sub_001D9290 → sub_00040660 →
 *   sub_0003C810 → sub_00040500 → sub_00189040 → sub_000405F0 →
 *   sub_0034CBF0 → sub_0034D530 → sub_0034C8A0 × 2 → sub_0034C2E0 →
 *   sub_001AD350 → sub_00188E10 → sub_0017F670 → sub_00031690 →
 *   sub_0018DE00 → sub_0017F6E0 → sub_0019A3E0 → sub_00189150 →
 *   sub_00188EE0
 *
 * Many of these (0x34xxxx-0x35xxxx) depend on MEM32(0x35FB48) and crash.
 * We call the safe ones and skip the D3D-dependent ones.
 * ================================================================= */
void sub_001AE6F0(void)
{
    static uint32_t call_count = 0;
    call_count++;

    /* stdcall: 2 params on stack: base_obj at [esp+4], phase at [esp+8] */
    uint32_t base_obj = MEM32(esp + 4);
    int32_t  phase    = (int32_t)MEM32(esp + 8);
    uint32_t render_base = base_obj + 0x12ADB0;
    int log = (call_count <= 20 || (call_count % 5000) == 0);

    /* Force device context pointer to correct value every frame.
     * Gen code writes mirror variant (0x35FBA8) during boot, shifting
     * all device field accesses by 0x2508. Reset to 0x35D6A0. */
    MEM32(0x35FB48) = 0x0035D6A0;

    if (log) {
        fprintf(stderr, "  [FE-DISPATCH] sub_001AE6F0 #%u: base=0x%X phase=%d\n",
                call_count, base_obj, phase);
        fprintf(stderr, "    render_list: [+0]=0x%08X [+4]=0x%08X [+8]=0x%08X\n",
                MEM32(render_base), MEM32(render_base + 4), MEM32(render_base + 8));
    }

    /* ── Call manual sub_0003FEE0 override ──
     * sub_0003FEE0 now has a manual override that skips D3D8LTCG state flush
     * calls but still does matrix copy, D3D clear, and render dispatch
     * (sub_001AD350 × 3 passes). */
    esi = 0x60E040;
    edi = 0x4D6170;
    ebx = 0;
    MEM32(0x60E170) = ebx;

    /* sub_00189040 — frontend screen widget renderer.
     * SKIPPED: walks pointer chains through frontend object (0x60E040)
     * that reference uninitialized screen widget data → MIRROR-FAIL flood.
     * This is the function that would generate D3D draw calls to populate
     * the device state tables, but it needs proper screen widget init. */

    /* sub_0034CBF0 — RT/DS setup + full render state flush.
     * Force device pointer before call. Pass 0,0 = use current RT/DS. */
    MEM32(0x35FB48) = 0x0035D6A0;
    PUSH32(esp, 0);  /* DS param = 0 (use current) */
    PUSH32(esp, 0);  /* RT param = 0 (use current) */
    PUSH32(esp, 0); sub_0034CBF0();

    /* sub_0003FEE0(game_obj=0x4D6170, scene_desc=zeroed) */
    {
        /* phase < 0: pass zeroed scene descriptor (safe path) */
        uint32_t zero_desc = esp - 0x10;
        MEM32(zero_desc) = 0;
        MEM32(zero_desc + 4) = 0;
        MEM32(zero_desc + 8) = 0;
        MEM32(zero_desc + 12) = 0;
        PUSH32(esp, zero_desc);
        PUSH32(esp, 0x4D6170);
        PUSH32(esp, 0); sub_0003FEE0();
    }

    MEM32(0x60E170) = 0xFFFFFFFFu;

    if (log)
        fprintf(stderr, "  [FE-DISPATCH] sub_001AE6F0 #%u DONE\n", call_count);

    /* stdcall: clean up 2 params (8 bytes) + return address */
    esp += 12; return; /* ret 8 */
}

/* sub_001AE732 - mid-function entry of sub_001AE6F0 (STUB)
 * Same native pointer problems. Just forward to sub_001AE6F0. */
void sub_001AE732(void)
{
    /* Push fake params for stdcall */
    PUSH32(esp, 0xFFFFFFFF); /* phase = -1 */
    PUSH32(esp, 0x60EA00);   /* base_obj */
    PUSH32(esp, 0);
    sub_001AE6F0();
}

/* =================================================================
 * sub_0034C2E0 - D3D Rendering State Machine (STUB)
 *
 * Original: 0x0034C2E0 - 0x00360A54 (83828 bytes, 20371 insns)
 * CC: cdecl, 6 params on stack, returns void
 *
 * The original reads MEM32(0x35FB48) which is a native heap pointer
 * (RW engine allocates the D3D state object on native heap).
 * MEM32() can't access native pointers → crash.
 *
 * Called from ~60 places in the recompiled code. Params are typically:
 * (flags, rect_count, clear_flags, color, depth, stencil)
 * We handle the "clear" case via our D3D8 layer in sub_00014D20.
 * All other calls are no-ops for now.
 * ================================================================= */
/* Write NV2A push buffer commands for Clear/Draw operations.
 * This is a minimal reimplementation that writes the commands
 * sub_0034D530's parser can pick up and translate to D3D11. */
/* Write a dword to the push buffer using the fixed address pointers */
static void pb_write_dword_fixed(uint32_t value)
{
    uint32_t write_ptr = MEM32(0x35D6A0);
    uint32_t end_ptr = MEM32(0x35D6A4);
    if (write_ptr + 4 < end_ptr) {
        MEM32(write_ptr) = value;
        MEM32(0x35D6A0) = write_ptr + 4;
    }
}

/* Push buffer command encoding */
#define PB_INC(method, count) (((count) << 18) | (method))
#define PB_NONINC(method, count) (0x40000000 | ((count) << 18) | (method))

void sub_0034C2E0(void)
{
    static uint32_t call_count = 0;
    call_count++;

    /* Read the 6 cdecl params: count, rects, flags, color, z, stencil */
    uint32_t p_count   = MEM32(esp + 4);
    uint32_t p_rects   = MEM32(esp + 8);
    uint32_t p_flags   = MEM32(esp + 12);
    uint32_t p_color   = MEM32(esp + 16);
    float    p_z       = MEMF(esp + 20);
    uint32_t p_stencil = MEM32(esp + 24);

    if (p_flags & 0xF3) {
        /* Write NV2A Clear commands to push buffer */
        pb_write_dword_fixed(PB_INC(0x01D4, 1));
        pb_write_dword_fixed(p_color);

        pb_write_dword_fixed(PB_INC(0x01D8, 2));
        pb_write_dword_fixed((640 << 16) | 0);
        pb_write_dword_fixed((480 << 16) | 0);

        pb_write_dword_fixed(PB_INC(0x01D0, 1));
        pb_write_dword_fixed(p_flags);
    }

    if (call_count <= 5 || (call_count % 50000) == 0) {
        fprintf(stderr, "  [D3D-SM] sub_0034C2E0 #%u: flags=0x%X color=0x%08X pb_write=0x%08X\n",
                call_count, p_flags, p_color, MEM32(0x35D6A0));
    }

    esp += 28; /* ret 24: pop return addr (4) + 6 cdecl params (24) */
    return;
}

/**
 * sub_00040820 - WaterFresnel texture array loader (STUB)
 *
 * Original: 0x00040820 - 0x00040855 (53 bytes) + tail call to
 *           sub_00040860 (0x00040860 - 0x00040AE0, 640 bytes)
 * CC: stdcall, 1 param (ret 4)
 *
 * Loads 17 WaterFresnel effect textures from "Graphics/%d.bum" files
 * into an array of raster pointers at the address given by the parameter.
 * Each iteration calls sub_00352560 (D3D surface creation), sub_001D2879
 * (RW raster alloc), locks the surface, reads data via ICALL vtable, and
 * memcpy's texture data.
 *
 * Crashes because: sub_001B33A0 (stream reader) is stubbed to mark-complete
 * without loading data, so D3D surface creation returns garbage pointers.
 * The memcpy (rep movsd) then reads from esi=0x80000000 (Xbox kernel range)
 * which is unmapped, causing an access violation in VCRUNTIME140.dll memcpy.
 *
 * Stub: zeroes the 17-pointer array (no water fresnel textures) and returns
 * success. The rendering pipeline should handle NULL raster pointers.
 */
void sub_00040820(void)
{
    /* Read parameter: address of 17-pointer array */
    uint32_t arr_va = MEM32(esp + 4);

    /* Zero out the 17 raster pointers (0x11 × 4 = 68 bytes) */
    for (int i = 0; i < 0x11; i++) {
        MEM32(arr_va + i * 4) = 0;
    }

    fprintf(stderr, "  [RW-TEX] sub_00040820: WaterFresnel stubbed, zeroed 17 ptrs at 0x%08X\n", arr_va);

    eax = 1;  /* success (al=1, matching gen code return) */
    esp += 8; return;  /* ret 4: pop return addr + 1 param */
}

/**
 * sub_00040860 - WaterFresnel inner loop (STUB)
 *
 * Original: 0x00040860 - 0x00040AE0 (640 bytes, 224 insns)
 * Tail-called from sub_00040820. Never reached with our override,
 * but declared to satisfy linker references.
 */
void sub_00040860(void)
{
    /* Should never be reached - sub_00040820 override returns directly */
    fprintf(stderr, "  [RW-TEX] sub_00040860: unexpected call (should not happen)\n");
    eax = 1;
    esp += 8; return;
}
