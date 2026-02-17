"""
Formulaic stub function classifier.

Identifies mechanically-generated stub functions:
- SSE float chains: sequences of movss/addss/subss/mulss/divss ending in ret
- These form the game's data-driven parameter initialization system:
  thousands of tiny functions that copy/compute float values between
  .rdata constants and .data globals.

General pattern: F3 0F XX 05 [addr] repeated N times, ending with C3 (ret).
Each SSE instruction is 8 bytes (prefix F3 0F + opcode + ModR/M 05 + 4-byte addr).
"""

from . import config

# SSE scalar float opcodes we recognize (after F3 0F prefix)
_SSE_OPCODES = {
    0x10,  # movss xmm, m32 (load)
    0x11,  # movss m32, xmm (store)
    0x58,  # addss
    0x59,  # mulss
    0x5C,  # subss
    0x5E,  # divss
    0x51,  # sqrtss
    0x5D,  # minss
    0x5F,  # maxss
}


def classify_stubs(xbe_data, functions, verbose=False):
    """
    Identify formulaic stub functions by byte-pattern matching.

    Args:
        xbe_data: Raw bytes of the entire XBE file.
        functions: List of function dicts.
        verbose: Print progress info.

    Returns:
        dict: func_addr (int) -> {
            "category": str,
            "stub_type": str,
            "confidence": float,
            "method": str
        }
    """
    results = {}
    by_type = {"float_copy": 0, "float_chain": 0, "double_op": 0}

    for func in functions:
        func_addr = int(func["start"], 16)
        func_size = func.get("size", 0)

        # Only check functions up to ~120 bytes (longest observed chains)
        if func_size < 9 or func_size > 120:
            continue

        file_offset = config.va_to_file_offset(func_addr)
        if file_offset is None or file_offset + func_size > len(xbe_data):
            continue

        func_bytes = xbe_data[file_offset:file_offset + func_size]

        # Check for SSE float chain: all 8-byte F3 0F XX 05 [addr] ops + C3
        if func_bytes[-1] == 0xC3 and func_bytes[0:2] == b'\xF3\x0F':
            if _is_sse_chain(func_bytes):
                n_ops = (func_size - 1) // 8
                if n_ops == 2:
                    stub_type = "float_copy"
                    by_type["float_copy"] += 1
                else:
                    stub_type = "float_chain"
                    by_type["float_chain"] += 1
                results[func_addr] = {
                    "category": "data_init",
                    "stub_type": stub_type,
                    "confidence": 0.99,
                    "method": "stub_pattern",
                }
                continue

        # F2 0F prefix = SSE2 double operations (same structure)
        if func_bytes[-1] == 0xC3 and func_bytes[0:2] == b'\xF2\x0F':
            if _is_sse_chain(func_bytes, double=True):
                by_type["double_op"] += 1
                results[func_addr] = {
                    "category": "data_init",
                    "stub_type": "double_op",
                    "confidence": 0.95,
                    "method": "stub_pattern",
                }
                continue

    if verbose:
        print(f"  Float copy stubs:    {by_type['float_copy']:,}")
        print(f"  Float chain stubs:   {by_type['float_chain']:,}")
        print(f"  Double op stubs:     {by_type['double_op']:,}")
        print(f"  Total data_init:     {len(results):,}")

    return results


def _is_sse_chain(func_bytes, double=False):
    """
    Check if function bytes form a chain of SSE scalar operations + ret.

    Each operation is 8 bytes: [F3/F2] 0F XX 05 [4-byte-addr]
    The chain ends with C3 (ret).
    Total size = N * 8 + 1 (where N >= 2).
    """
    body_len = len(func_bytes) - 1  # exclude final C3
    if body_len % 8 != 0:
        return False
    if body_len < 16:  # need at least 2 ops
        return False

    prefix = b'\xF2' if double else b'\xF3'

    for i in range(0, body_len, 8):
        if func_bytes[i] != prefix[0]:
            return False
        if func_bytes[i + 1] != 0x0F:
            return False
        opcode = func_bytes[i + 2]
        if opcode not in _SSE_OPCODES:
            return False
        if func_bytes[i + 3] != 0x05:
            return False
        # bytes 4-7 are the address (any value is fine)

    return True
