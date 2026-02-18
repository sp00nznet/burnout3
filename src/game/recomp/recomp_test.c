/**
 * Burnout 3: Takedown - Recompiled Code Integration Test
 *
 * Tests the full pipeline:
 *   1. Xbox memory layout initialized (data sections at original VAs)
 *   2. Translated C functions can read/write Xbox global memory
 *   3. Calling convention works for different function types
 *
 * This file contains hand-translated test functions and a harness
 * that verifies they produce correct results.
 */

#include "recomp_types.h"
#include "recomp_dispatch.h"
#include "../../kernel/xbox_memory_layout.h"
#include <stdio.h>

/* ── Test: Read .rdata constant ─────────────────────────────
 *
 * Many data_init functions just copy a float from .rdata to .data:
 *   movss xmm0, [rdata_addr]
 *   movss [data_addr], xmm0
 *   ret
 *
 * Example: sub_002575A0 copies MEMF(0x3B191C) → MEMF(0x4D53CC)
 */
static int test_rdata_to_data_copy(void)
{
    float src_val, dst_val;

    /* Read the source value from .rdata (set up by xbox_MemoryLayoutInit) */
    src_val = MEMF(0x3B191C);

    /* Clear destination first */
    MEMF(0x4D53CC) = 0.0f;

    /* Execute the translated function logic */
    {
        float xmm0;
        xmm0 = MEMF(0x3B191C);
        MEMF(0x4D53CC) = xmm0;
    }

    /* Verify */
    dst_val = MEMF(0x4D53CC);
    if (dst_val == src_val) {
        fprintf(stderr, "  PASS: .rdata→.data copy (0x3B191C→0x4D53CC = %f)\n", dst_val);
        return 1;
    } else {
        fprintf(stderr, "  FAIL: .rdata→.data copy (expected %f, got %f)\n", src_val, dst_val);
        return 0;
    }
}

/* ── Test: Integer global read/write ────────────────────────
 *
 * Test that MEM32 macros correctly access Xbox memory addresses.
 */
static int test_integer_globals(void)
{
    uint32_t test_val = 0xDEADBEEF;
    uint32_t read_back;

    /* Write to a .data address (BSS region, should be writable) */
    MEM32(0x004D5000) = test_val;
    read_back = MEM32(0x004D5000);

    if (read_back == test_val) {
        fprintf(stderr, "  PASS: MEM32 read/write at 0x004D5000 = 0x%08X\n", read_back);
        return 1;
    } else {
        fprintf(stderr, "  FAIL: MEM32 expected 0x%08X, got 0x%08X\n", test_val, read_back);
        return 0;
    }
}

/* ── Test: Byte/word access ─────────────────────────────────
 *
 * Test sub-dword memory access macros.
 */
static int test_byte_word_access(void)
{
    int pass = 1;

    /* Write a known pattern */
    MEM32(0x004D5010) = 0x11223344;

    if (MEM8(0x004D5010) != 0x44) {  /* x86 is little-endian */
        fprintf(stderr, "  FAIL: MEM8 low byte\n");
        pass = 0;
    }
    if (MEM16(0x004D5010) != 0x3344) {
        fprintf(stderr, "  FAIL: MEM16 low word\n");
        pass = 0;
    }
    if (MEM8(0x004D5013) != 0x11) {
        fprintf(stderr, "  FAIL: MEM8 high byte\n");
        pass = 0;
    }

    /* Test byte write */
    MEM8(0x004D5010) = 0xFF;
    if (MEM32(0x004D5010) != 0x112233FF) {
        fprintf(stderr, "  FAIL: MEM8 write (got 0x%08X)\n", MEM32(0x004D5010));
        pass = 0;
    }

    if (pass)
        fprintf(stderr, "  PASS: byte/word memory access\n");
    return pass;
}

/* ── Test: Register macros ──────────────────────────────────
 *
 * Test the LO8/HI8/LO16/SET_* register helper macros.
 */
static int test_register_macros(void)
{
    uint32_t eax = 0xAABBCCDD;
    int pass = 1;

    if (LO8(eax) != 0xDD) { fprintf(stderr, "  FAIL: LO8\n"); pass = 0; }
    if (HI8(eax) != 0xCC) { fprintf(stderr, "  FAIL: HI8\n"); pass = 0; }
    if (LO16(eax) != 0xCCDD) { fprintf(stderr, "  FAIL: LO16\n"); pass = 0; }

    SET_LO8(eax, 0x11);
    if (eax != 0xAABBCC11) { fprintf(stderr, "  FAIL: SET_LO8 (0x%08X)\n", eax); pass = 0; }

    SET_HI8(eax, 0x22);
    if (eax != 0xAABB2211) { fprintf(stderr, "  FAIL: SET_HI8 (0x%08X)\n", eax); pass = 0; }

    SET_LO16(eax, 0x5566);
    if (eax != 0xAABB5566) { fprintf(stderr, "  FAIL: SET_LO16 (0x%08X)\n", eax); pass = 0; }

    if (pass)
        fprintf(stderr, "  PASS: register macros\n");
    return pass;
}

/* ── Test: Comparison macros ────────────────────────────────
 *
 * Test the CMP and TEST macros used by translated jcc instructions.
 */
static int test_comparison_macros(void)
{
    int pass = 1;

    /* Unsigned comparisons */
    if (!CMP_EQ(5, 5))  { fprintf(stderr, "  FAIL: CMP_EQ\n"); pass = 0; }
    if (!CMP_NE(5, 6))  { fprintf(stderr, "  FAIL: CMP_NE\n"); pass = 0; }
    if (!CMP_B(3, 5))   { fprintf(stderr, "  FAIL: CMP_B\n"); pass = 0; }
    if (!CMP_A(5, 3))   { fprintf(stderr, "  FAIL: CMP_A\n"); pass = 0; }
    if (!CMP_AE(5, 5))  { fprintf(stderr, "  FAIL: CMP_AE\n"); pass = 0; }
    if (!CMP_BE(5, 5))  { fprintf(stderr, "  FAIL: CMP_BE\n"); pass = 0; }

    /* Signed comparisons */
    if (!CMP_L(-1, 1))  { fprintf(stderr, "  FAIL: CMP_L\n"); pass = 0; }
    if (!CMP_G(1, -1))  { fprintf(stderr, "  FAIL: CMP_G\n"); pass = 0; }
    if (!CMP_GE(0, 0))  { fprintf(stderr, "  FAIL: CMP_GE\n"); pass = 0; }
    if (!CMP_LE(0, 0))  { fprintf(stderr, "  FAIL: CMP_LE\n"); pass = 0; }

    /* Unsigned: 0xFFFFFFFF should be > 0 */
    if (!CMP_A(0xFFFFFFFF, 0)) { fprintf(stderr, "  FAIL: CMP_A unsigned\n"); pass = 0; }
    /* Signed: 0xFFFFFFFF is -1, should be < 0 */
    if (!CMP_L(0xFFFFFFFF, 0)) { fprintf(stderr, "  FAIL: CMP_L signed\n"); pass = 0; }

    /* TEST macros */
    if (!TEST_Z(0xFF00, 0x00FF))  { fprintf(stderr, "  FAIL: TEST_Z\n"); pass = 0; }
    if (!TEST_NZ(0xFF00, 0x0F00)) { fprintf(stderr, "  FAIL: TEST_NZ\n"); pass = 0; }

    if (pass)
        fprintf(stderr, "  PASS: comparison macros\n");
    return pass;
}

/* ── Test: Stack simulation ─────────────────────────────────
 *
 * Test PUSH32/POP32 macros with a simulated stack in Xbox memory.
 */
static int test_stack_simulation(void)
{
    /* Use a high .data BSS address as a stack */
    uint32_t esp = 0x00700000;
    uint32_t val;
    int pass = 1;

    PUSH32(esp, 0x12345678);
    PUSH32(esp, 0xAABBCCDD);

    if (esp != 0x006FFFF8) {
        fprintf(stderr, "  FAIL: esp after 2 pushes (0x%08X)\n", esp);
        pass = 0;
    }

    POP32(esp, val);
    if (val != 0xAABBCCDD) {
        fprintf(stderr, "  FAIL: POP32 first (0x%08X)\n", val);
        pass = 0;
    }

    POP32(esp, val);
    if (val != 0x12345678) {
        fprintf(stderr, "  FAIL: POP32 second (0x%08X)\n", val);
        pass = 0;
    }

    if (esp != 0x00700000) {
        fprintf(stderr, "  FAIL: esp after 2 pops (0x%08X)\n", esp);
        pass = 0;
    }

    if (pass)
        fprintf(stderr, "  PASS: stack simulation\n");
    return pass;
}

/* ── Test: Call actual translated function (float copy) ────
 *
 * Calls sub_002575A0 which does:
 *   xmm0 = MEMF(0x3B191C);  // read from .rdata
 *   MEMF(0x4D53CC) = xmm0;  // write to .data BSS
 */
extern void sub_002575A0(void);

static int test_call_translated_float_copy(void)
{
    float src_val, dst_val;

    /* Read the source value from .rdata */
    src_val = MEMF(0x3B191C);

    /* Clear destination */
    MEMF(0x4D53CC) = 0.0f;

    /* Call the ACTUAL translated function */
    sub_002575A0();

    /* Verify destination was written */
    dst_val = MEMF(0x4D53CC);
    if (dst_val == src_val) {
        fprintf(stderr, "  PASS: translated sub_002575A0 (float copy: %f)\n", dst_val);
        return 1;
    } else {
        fprintf(stderr, "  FAIL: translated sub_002575A0 (expected %f, got %f)\n",
                src_val, dst_val);
        return 0;
    }
}

/* ── Test: Call translated function (float chain: subtract) ──
 *
 * Calls sub_00257720 which does:
 *   xmm0 = MEMF(0x4D53F8);          // read from .data
 *   xmm0 = xmm0 - MEMF(0x3A7964);  // subtract .rdata constant
 *   MEMF(0x4D5408) = xmm0;          // write result to .data
 */
extern void sub_00257720(void);

static int test_call_translated_float_chain(void)
{
    float input_val, sub_val, expected, result;

    /* Set up a known input value in .data */
    MEMF(0x4D53F8) = 100.0f;

    /* Read the .rdata subtract constant */
    sub_val = MEMF(0x3A7964);

    /* Clear destination */
    MEMF(0x4D5408) = 0.0f;

    /* Call the ACTUAL translated function */
    sub_00257720();

    /* Verify: result should be 100.0 - sub_val */
    input_val = 100.0f;
    expected = input_val - sub_val;
    result = MEMF(0x4D5408);

    if (result == expected) {
        fprintf(stderr, "  PASS: translated sub_00257720 (100.0 - %f = %f)\n",
                sub_val, result);
        return 1;
    } else {
        fprintf(stderr, "  FAIL: translated sub_00257720 (expected %f, got %f)\n",
                expected, result);
        return 0;
    }
}

/* ── Test: Dispatch table lookup ──────────────────────────
 *
 * Verify that recomp_lookup() can find translated functions
 * by their original Xbox virtual address.
 */
static int test_dispatch_lookup(void)
{
    recomp_func_t func;
    size_t count;
    int pass = 1;

    count = recomp_get_count();
    if (count == 0) {
        fprintf(stderr, "  FAIL: dispatch table is empty\n");
        return 0;
    }

    /* Look up sub_002575A0 */
    func = recomp_lookup(0x002575A0);
    if (!func) {
        fprintf(stderr, "  FAIL: recomp_lookup(0x002575A0) returned NULL\n");
        pass = 0;
    } else if (func != (recomp_func_t)sub_002575A0) {
        fprintf(stderr, "  FAIL: recomp_lookup(0x002575A0) returned wrong pointer\n");
        pass = 0;
    }

    /* Look up a non-existent address */
    func = recomp_lookup(0x00000001);
    if (func != NULL) {
        fprintf(stderr, "  FAIL: recomp_lookup(0x00000001) should be NULL\n");
        pass = 0;
    }

    if (pass)
        fprintf(stderr, "  PASS: dispatch table (%zu functions registered)\n", count);
    return pass;
}

/* ── Test: Bulk-execute ALL data_init functions ────────────
 *
 * Call all 13,868 translated data_init functions and verify
 * they don't crash. This is the ultimate pipeline test.
 */

/* Defined in recomp_dispatch.c - we need direct table access for bulk test */
extern size_t recomp_call_all(void);

static int test_bulk_data_init(void)
{
    size_t count = recomp_get_count();
    size_t called;

    if (count == 0) {
        fprintf(stderr, "  FAIL: no functions in dispatch table\n");
        return 0;
    }

    called = recomp_call_all();

    if (called == count) {
        fprintf(stderr, "  PASS: executed all %zu translated functions without crash\n",
                called);
        return 1;
    } else {
        fprintf(stderr, "  FAIL: only called %zu of %zu functions\n", called, count);
        return 0;
    }
}

/* ── Test runner ────────────────────────────────────────── */

int recomp_run_tests(void)
{
    int passed = 0;
    int total = 0;

    fprintf(stderr, "\n=== Recompiled Code Integration Tests ===\n");

    /* Verify Xbox memory is initialized */
    if (!xbox_GetMemoryBase()) {
        fprintf(stderr, "SKIP: Xbox memory layout not initialized\n");
        return -1;
    }

    /* Core macro tests */
    total++; passed += test_integer_globals();
    total++; passed += test_byte_word_access();
    total++; passed += test_register_macros();
    total++; passed += test_comparison_macros();
    total++; passed += test_stack_simulation();
    total++; passed += test_rdata_to_data_copy();

    /* Translated function execution tests */
    total++; passed += test_call_translated_float_copy();
    total++; passed += test_call_translated_float_chain();
    total++; passed += test_dispatch_lookup();
    total++; passed += test_bulk_data_init();

    fprintf(stderr, "\n=== Results: %d/%d tests passed ===\n\n", passed, total);
    return (passed == total) ? 0 : 1;
}
