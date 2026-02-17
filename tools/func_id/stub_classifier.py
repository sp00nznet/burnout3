"""
Formulaic stub function classifier.

Identifies mechanically-generated stub functions:
- SSE float copy stubs (movss load, movss store, ret) - 17 bytes, 3 instrs
- SSE float compute stubs (movss load, SSE op, movss store, ret) - 25 bytes, 4 instrs
- Other small formulaic patterns

These are the game's data-driven parameter initialization system:
thousands of tiny functions that copy/compute float values between
.rdata constants and .data globals.
"""

from . import config


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
    float_copy_count = 0
    float_compute_count = 0

    for func in functions:
        func_addr = int(func["start"], 16)
        func_size = func.get("size", 0)
        num_instrs = func.get("num_instructions", 0)

        # Only check small functions
        if func_size > 40 or num_instrs > 6:
            continue

        file_offset = config.va_to_file_offset(func_addr)
        if file_offset is None or file_offset + func_size > len(xbe_data):
            continue

        func_bytes = xbe_data[file_offset:file_offset + func_size]

        # Pattern 1: SSE float copy (17 bytes, 3 instructions)
        # F3 0F 10 05 [addr]  ; movss xmm0, [addr]
        # F3 0F 11 05 [addr]  ; movss [addr], xmm0
        # C3                  ; ret
        if func_size == 17 and num_instrs == 3:
            if (func_bytes[0:4] == b'\xF3\x0F\x10\x05' and
                    func_bytes[8:12] == b'\xF3\x0F\x11\x05' and
                    func_bytes[16] == 0xC3):
                results[func_addr] = {
                    "category": "data_init",
                    "stub_type": "float_copy",
                    "confidence": 0.99,
                    "method": "stub_pattern",
                }
                float_copy_count += 1
                continue

        # Pattern 2: SSE float compute (25 bytes, 4 instructions)
        # F3 0F 10 05 [addr]  ; movss xmm0, [addr]
        # F3 0F XX 05 [addr]  ; SSE op (addss/subss/mulss/divss)
        # F3 0F 11 05 [addr]  ; movss [addr], xmm0
        # C3                  ; ret
        if func_size == 25 and num_instrs == 4:
            if (func_bytes[0:4] == b'\xF3\x0F\x10\x05' and
                    func_bytes[8:10] == b'\xF3\x0F' and
                    func_bytes[11] == 0x05 and
                    func_bytes[16:20] == b'\xF3\x0F\x11\x05' and
                    func_bytes[24] == 0xC3):
                results[func_addr] = {
                    "category": "data_init",
                    "stub_type": "float_compute",
                    "confidence": 0.99,
                    "method": "stub_pattern",
                }
                float_compute_count += 1
                continue

        # Pattern 3: SSE double operations (similar but with F2 prefix)
        if func_size <= 33 and num_instrs <= 4:
            if (func_bytes[0:2] == b'\xF2\x0F' and
                    func_bytes[-1] == 0xC3):
                results[func_addr] = {
                    "category": "data_init",
                    "stub_type": "double_op",
                    "confidence": 0.95,
                    "method": "stub_pattern",
                }
                continue

    if verbose:
        print(f"  Float copy stubs:    {float_copy_count:,}")
        print(f"  Float compute stubs: {float_compute_count:,}")
        print(f"  Total data_init:     {len(results):,}")

    return results
