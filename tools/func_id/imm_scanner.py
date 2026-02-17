"""
Immediate operand scanner for .text section bytes.

Scans raw x86 bytes for `push imm32` and `mov reg, imm32` instructions
whose immediate values fall within .rdata (or .data) address ranges.
This captures string/data references that the xref database misses
because it only tracked CS_OP_MEM operands, not CS_OP_IMM.
"""

import struct
from collections import defaultdict

from . import config


def scan_immediate_refs(xbe_data, functions, verbose=False):
    """
    Single-pass scan of the .text section bytes for immediate operands
    referencing .rdata addresses.

    Args:
        xbe_data: Raw bytes of the entire XBE file.
        functions: List of function dicts (with 'start' hex string keys).
        verbose: Print progress info.

    Returns:
        dict: rdata_addr (int) -> list of func_start_addr (int)
              Maps each .rdata address found as an immediate operand
              to the function(s) containing that reference.
    """
    # Build sorted function start list for binary search lookup
    func_starts = sorted(int(f["start"], 16) for f in functions)

    # Section boundaries
    text_file_start = config.TEXT_RAW_ADDR
    text_file_end = text_file_start + config.TEXT_VA_SIZE
    text_bytes = xbe_data[text_file_start:text_file_end]
    text_va_base = config.TEXT_VA_START

    rdata_lo = config.RDATA_VA_START
    rdata_hi = config.RDATA_VA_END
    data_lo = config.DATA_VA_START
    data_hi = config.DATA_VA_END

    if verbose:
        print(f"  Scanning {len(text_bytes):,} bytes of .text section...")
        print(f"  .rdata range: 0x{rdata_lo:08X} - 0x{rdata_hi:08X}")

    # Results: rdata_addr -> set of code addresses that reference it
    refs_by_rdata_addr = defaultdict(set)
    total_refs = 0

    # Single-pass scan
    i = 0
    end = len(text_bytes) - 5  # Need at least opcode + 4 bytes

    while i < end:
        b = text_bytes[i]
        imm_val = None

        # push imm32: opcode 0x68, followed by 4-byte LE immediate
        if b == 0x68:
            imm_val = struct.unpack_from("<I", text_bytes, i + 1)[0]
            if _in_data_range(imm_val, rdata_lo, rdata_hi, data_lo, data_hi):
                code_va = text_va_base + i
                refs_by_rdata_addr[imm_val].add(code_va)
                total_refs += 1
            i += 5
            continue

        # mov reg, imm32: opcodes 0xB8-0xBF (mov eax/ecx/edx/ebx/esp/ebp/esi/edi, imm32)
        if 0xB8 <= b <= 0xBF:
            imm_val = struct.unpack_from("<I", text_bytes, i + 1)[0]
            if _in_data_range(imm_val, rdata_lo, rdata_hi, data_lo, data_hi):
                code_va = text_va_base + i
                refs_by_rdata_addr[imm_val].add(code_va)
                total_refs += 1
            i += 5
            continue

        # mov r/m32, imm32 with Mod R/M: opcode 0xC7 /0
        # e.g., mov [ebp-XX], imm32 or mov dword ptr [addr], imm32
        # We only care about the imm32 part, but the instruction length varies
        # Skip this for now - push/mov-reg cover most string refs

        i += 1

    if verbose:
        print(f"  Found {total_refs:,} immediate references to .rdata/.data")
        print(f"  Unique .rdata/.data addresses referenced: {len(refs_by_rdata_addr):,}")

    # Map code addresses to their containing functions
    rdata_to_funcs = defaultdict(set)
    for rdata_addr, code_addrs in refs_by_rdata_addr.items():
        for code_addr in code_addrs:
            func_addr = _find_containing_function(code_addr, func_starts)
            if func_addr is not None:
                rdata_to_funcs[rdata_addr].add(func_addr)

    if verbose:
        n_funcs = len(set().union(*rdata_to_funcs.values())) if rdata_to_funcs else 0
        print(f"  Mapped to {n_funcs:,} unique functions")

    # Convert sets to sorted lists for JSON serialization
    return {addr: sorted(funcs) for addr, funcs in rdata_to_funcs.items()}


def _in_data_range(val, rdata_lo, rdata_hi, data_lo, data_hi):
    """Check if an immediate value falls in .rdata or .data VA range."""
    return (rdata_lo <= val < rdata_hi) or (data_lo <= val < data_hi)


def _find_containing_function(code_addr, sorted_func_starts):
    """
    Binary search to find which function contains code_addr.
    Returns the function start address, or None if not found.
    """
    lo, hi = 0, len(sorted_func_starts) - 1
    while lo <= hi:
        mid = (lo + hi) // 2
        if sorted_func_starts[mid] <= code_addr:
            lo = mid + 1
        else:
            hi = mid - 1
    # hi now points to the last func_start <= code_addr
    if hi >= 0:
        return sorted_func_starts[hi]
    return None
