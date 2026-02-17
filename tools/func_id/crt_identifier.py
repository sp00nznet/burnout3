"""
CRT/MSVC runtime function identification via byte-level signatures.

Matches the first N bytes of each function against curated patterns
for common MSVC CRT functions (memcpy, strlen, _ftol, _chkstk, etc.).
"""

from . import config


def identify_crt_functions(xbe_data, functions, verbose=False):
    """
    Identify CRT functions by matching byte signatures.

    Args:
        xbe_data: Raw bytes of the entire XBE file.
        functions: List of function dicts.
        verbose: Print progress info.

    Returns:
        dict: func_addr (int) -> {
            "name": str,
            "confidence": float,
            "method": "crt_signature",
            "pattern_len": int
        }
    """
    results = {}
    # Track which signature names have been matched to avoid duplicates
    # (e.g., _chkstk and _alloca_probe share the same bytes)
    matched_names = {}

    if verbose:
        print(f"  Matching {len(config.CRT_SIGNATURES)} CRT signatures against {len(functions)} functions...")

    for func in functions:
        func_addr = int(func["start"], 16)
        func_size = func.get("size", 0)

        # Read first 32 bytes of the function from XBE
        file_offset = config.va_to_file_offset(func_addr)
        if file_offset is None:
            continue

        # Read enough bytes for signature matching
        read_len = min(32, len(xbe_data) - file_offset)
        if read_len < 2:
            continue
        func_bytes = xbe_data[file_offset:file_offset + read_len]

        # Try each signature
        for sig_name, pattern, mask, max_size in config.CRT_SIGNATURES:
            if len(func_bytes) < len(pattern):
                continue

            # Size check: if max_size > 0, function shouldn't be too large
            if max_size > 0 and func_size > max_size * 2:
                continue

            if _match_pattern(func_bytes, pattern, mask):
                # Prefer longer pattern matches (more specific)
                if func_addr in results:
                    if len(pattern) <= results[func_addr]["pattern_len"]:
                        continue

                # If this name is already matched to a different address,
                # keep the one with smaller function size (more likely correct)
                if sig_name in matched_names:
                    prev_addr = matched_names[sig_name]
                    prev_size = results[prev_addr].get("func_size", 0)
                    if func_size > 0 and prev_size > 0 and func_size >= prev_size:
                        continue
                    # New match is better, remove old one
                    del results[prev_addr]

                results[func_addr] = {
                    "name": sig_name,
                    "confidence": config.CONFIDENCE_CRT_SIGNATURE,
                    "method": "crt_signature",
                    "pattern_len": len(pattern),
                    "func_size": func_size,
                }
                matched_names[sig_name] = func_addr

    # Clean up internal fields
    for r in results.values():
        del r["func_size"]

    if verbose:
        print(f"  Identified {len(results)} CRT functions")
        for addr in sorted(results):
            print(f"    0x{addr:08X}: {results[addr]['name']}")

    return results


def _match_pattern(func_bytes, pattern, mask):
    """Match a byte pattern against function bytes, with optional mask."""
    if mask is None:
        return func_bytes[:len(pattern)] == pattern
    for i in range(len(pattern)):
        if mask[i] == 0xFF:
            if func_bytes[i] != pattern[i]:
                return False
        elif mask[i] != 0x00:
            if (func_bytes[i] & mask[i]) != (pattern[i] & mask[i]):
                return False
    return True
