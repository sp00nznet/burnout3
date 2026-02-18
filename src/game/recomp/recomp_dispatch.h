/**
 * Burnout 3: Takedown - Recompiled Function Dispatch
 *
 * Provides lookup from original Xbox virtual addresses to
 * translated C function pointers. Used for:
 * - Indirect call resolution (call eax, call [vtable + offset])
 * - Integration testing (call specific functions by VA)
 * - Runtime verification of translation coverage
 */

#ifndef RECOMP_DISPATCH_H
#define RECOMP_DISPATCH_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Generic function pointer type for dispatch */
typedef void (*recomp_func_t)(void);

/**
 * Look up a translated function by its original Xbox VA.
 * Returns NULL if the function has not been translated.
 * O(log n) binary search.
 */
recomp_func_t recomp_lookup(uint32_t xbox_va);

/**
 * Get the total number of translated functions registered
 * in the dispatch table.
 */
size_t recomp_get_count(void);

/**
 * Call all registered functions in order.
 * Used for bulk testing of data_init functions.
 * Returns the number of functions successfully called.
 */
size_t recomp_call_all(void);

#ifdef __cplusplus
}
#endif

#endif /* RECOMP_DISPATCH_H */
