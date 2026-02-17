"""
ABI and calling convention analyzer for Burnout 3.

Examines function prologues, epilogues, and parameter access patterns
to determine:
- Calling convention (cdecl, thiscall, fastcall, custom/LTCG)
- Frame pointer usage (EBP frame vs FPO)
- Parameter count estimates
- Register preservation patterns
- Return type hints (void, int, float, pointer)

This information is critical for correct decompilation.
"""

import json
import os
import struct
import time
from collections import Counter, defaultdict

# Section info
TEXT_VA_START = 0x00011000
TEXT_VA_SIZE = 2863616
TEXT_RAW_ADDR = 0x00001000

# Default paths
DEFAULT_FUNCTIONS_JSON = "tools/disasm/output/functions.json"
DEFAULT_IDENTIFIED_JSON = "tools/func_id/output/identified_functions.json"
DEFAULT_OUTPUT_DIR = "tools/abi_analysis/output"

# Maximum bytes to read from each function for prologue/epilogue analysis
MAX_PROLOGUE_BYTES = 64
MAX_EPILOGUE_BYTES = 32


def run(xbe_path, functions_path=None, identified_path=None,
        output_dir=None, verbose=False):
    """Run the ABI analysis pipeline."""
    functions_path = functions_path or DEFAULT_FUNCTIONS_JSON
    identified_path = identified_path or DEFAULT_IDENTIFIED_JSON
    output_dir = output_dir or DEFAULT_OUTPUT_DIR

    t_start = time.time()

    if verbose:
        print("Loading inputs...")

    xbe_data = _load_binary(xbe_path)
    functions = _load_json(functions_path)
    identified = _load_json(identified_path)

    # Build category lookup
    func_cat = {}
    for f in identified:
        func_cat[f["start"]] = f.get("category", "unknown")

    if verbose:
        print(f"  Functions to analyze: {len(functions):,}")

    # Phase 1: Analyze each function
    if verbose:
        print("\nPhase 1: Prologue/epilogue analysis...")
    t1 = time.time()

    abi_results = []
    for f in functions:
        cat = func_cat.get(f["start"], "unknown")
        if cat == "data_init":
            continue  # Skip SSE parameter stubs

        result = _analyze_function(f, xbe_data, cat)
        if result:
            abi_results.append(result)

    if verbose:
        print(f"  Analyzed: {len(abi_results):,} functions")
        print(f"  Done in {time.time() - t1:.1f}s")

    # Phase 2: Build statistics
    if verbose:
        print("\nPhase 2: Building statistics...")

    stats = _build_statistics(abi_results)

    if verbose:
        _print_statistics(stats)

    # Phase 3: Write output
    if verbose:
        print("\nPhase 3: Writing output...")

    os.makedirs(output_dir, exist_ok=True)
    _write_json(os.path.join(output_dir, "abi_functions.json"), abi_results)
    _write_json(os.path.join(output_dir, "abi_summary.json"), stats)

    if verbose:
        print(f"\nTotal time: {time.time() - t_start:.1f}s")
        print(f"Output written to: {output_dir}/")


def _analyze_function(func, xbe_data, category):
    """Analyze a single function's ABI characteristics."""
    addr = int(func["start"], 16)
    size = func["size"]

    if size < 1:
        return None

    offset = _va_to_file_offset(addr)
    if offset is None:
        return None

    # Read function bytes
    read_size = min(size, MAX_PROLOGUE_BYTES)
    if offset + read_size > len(xbe_data):
        return None

    prologue = xbe_data[offset:offset + read_size]

    # Read epilogue (last N bytes)
    epi_size = min(size, MAX_EPILOGUE_BYTES)
    epi_offset = offset + size - epi_size
    if epi_offset < 0 or epi_offset + epi_size > len(xbe_data):
        epilogue = b''
    else:
        epilogue = xbe_data[epi_offset:epi_offset + epi_size]

    result = {
        "address": func["start"],
        "size": size,
        "category": category,
    }

    # Determine frame type
    result["frame_type"] = _detect_frame_type(prologue)

    # Detect calling convention
    result["calling_convention"] = _detect_calling_convention(
        prologue, epilogue, size
    )

    # Estimate parameter count
    result["estimated_params"] = _estimate_params(prologue, size, result["frame_type"])

    # Detect preserved registers
    result["preserved_regs"] = _detect_preserved_registers(prologue, epilogue)

    # Return type hints
    result["return_hint"] = _detect_return_type(epilogue)

    # Stack frame size
    result["stack_frame_size"] = _detect_stack_size(prologue, result["frame_type"])

    return result


def _detect_frame_type(prologue):
    """Detect whether function uses EBP frame or FPO."""
    if len(prologue) >= 3 and prologue[:3] == b'\x55\x8B\xEC':
        return "ebp_frame"

    # sub esp, N without push ebp = FPO with stack allocation
    if len(prologue) >= 3:
        if prologue[0] == 0x83 and prologue[1] == 0xEC:
            return "fpo_stack"
        if prologue[:2] == b'\x81\xEC':
            return "fpo_stack"

    return "fpo_leaf"


def _detect_calling_convention(prologue, epilogue, func_size):
    """
    Detect calling convention from prologue/epilogue patterns.

    Key indicators:
    - thiscall: ECX used as base pointer (mov reg, [ecx+N])
    - fastcall: ECX/EDX used for first two params
    - cdecl: parameters accessed via [esp+N] or [ebp+N]
    - stdcall: ret N at end (callee cleans stack)
    """
    # Check epilogue for ret N (stdcall/thiscall with stack cleanup)
    has_ret_n = False
    stack_cleanup = 0
    if len(epilogue) >= 3:
        # Look for C2 XX XX (ret imm16) in last 3 bytes
        for i in range(len(epilogue) - 3, max(len(epilogue) - 8, -1), -1):
            if i >= 0 and epilogue[i] == 0xC2:
                has_ret_n = True
                stack_cleanup = struct.unpack_from('<H', epilogue, i + 1)[0]
                break

    # Check for thiscall indicators in prologue
    # Patterns: mov [esp+N], ecx / mov reg, ecx / push ecx early / mov reg, [ecx+N]
    ecx_usage = _check_ecx_as_this(prologue)

    if ecx_usage and has_ret_n:
        return "thiscall"  # ECX = this, callee cleans stack
    elif ecx_usage:
        return "thiscall_cdecl"  # ECX = this, caller cleans (MSVC thiscall)
    elif has_ret_n:
        return "stdcall"
    else:
        return "cdecl"


def _check_ecx_as_this(prologue):
    """Check if ECX is used as a 'this' pointer in the prologue."""
    if len(prologue) < 4:
        return False

    for i in range(min(len(prologue) - 2, 32)):
        b0 = prologue[i]
        b1 = prologue[i + 1] if i + 1 < len(prologue) else 0

        # mov reg, [ecx+disp8]: 8B XX where XX mod/rm indicates [ecx+N]
        # ModR/M: mod=01, r/m=001 (ECX) -> XX = 0x41, 0x49, 0x51, 0x59, 0x61, 0x69, 0x71, 0x79
        if b0 == 0x8B and (b1 & 0xC7) == 0x41:
            return True

        # mov reg, [ecx]: 8B XX where mod=00, r/m=001
        # XX = 0x01, 0x09, 0x11, 0x19, 0x21, 0x29, 0x31, 0x39
        if b0 == 0x8B and (b1 & 0xC7) == 0x01:
            return True

        # mov [ecx+disp8], reg (store to this->field)
        if b0 == 0x89 and (b1 & 0xC7) == 0x41:
            return True

        # mov eax, ecx (8B C1) or mov esi, ecx (8B F1) etc.
        if b0 == 0x8B and (b1 & 0x07) == 0x01 and (b1 & 0xC0) == 0xC0:
            return True

        # push ecx (51) in first 3 bytes as register save
        if b0 == 0x51 and i < 3:
            # Only if followed by something that accesses ECX
            continue

    return False


def _estimate_params(prologue, func_size, frame_type):
    """
    Estimate parameter count from stack access patterns.

    For EBP frames: look for [ebp+08], [ebp+0C], [ebp+10], etc.
    For FPO: look for [esp+XX] with highest offset in early code.
    """
    if func_size < 4:
        return 0

    max_param_offset = 0

    if frame_type == "ebp_frame":
        # Scan for [ebp+XX] where XX >= 8 (parameters start at ebp+8)
        for i in range(min(len(prologue) - 2, 48)):
            # [ebp+disp8]: mod=01, r/m=101 -> modrm & 0x47 == 0x45
            if prologue[i + 1] & 0x47 == 0x45 if i + 1 < len(prologue) else False:
                if i + 2 < len(prologue):
                    disp = prologue[i + 2]
                    if 8 <= disp <= 0x40:
                        max_param_offset = max(max_param_offset, disp)

    elif frame_type == "fpo_stack":
        # For FPO, harder to determine - would need stack size knowledge
        # Just look for high [esp+XX] accesses
        pass

    if max_param_offset >= 8:
        return (max_param_offset - 4) // 4  # Each param is 4 bytes
    return 0


def _detect_preserved_registers(prologue, epilogue):
    """Detect which callee-saved registers are preserved."""
    preserved = []

    # Standard callee-saved: EBX, ESI, EDI, EBP
    # Look for push/pop patterns
    push_regs = {0x53: "ebx", 0x56: "esi", 0x57: "edi", 0x55: "ebp"}

    for i in range(min(len(prologue), 8)):
        b = prologue[i]
        if b in push_regs:
            preserved.append(push_regs[b])
        elif b not in (0x8B, 0x83, 0x81, 0x89, 0x50, 0x51, 0x52):
            # Stop at first non-push/non-standard instruction
            break

    return preserved


def _detect_return_type(epilogue):
    """
    Hint at return type from epilogue pattern.

    - fld/fstp before ret -> float return
    - xor eax, eax before ret -> returns 0 (bool/int)
    - mov eax, [addr] before ret -> returns pointer/value
    - plain ret -> void or value already in EAX
    """
    if len(epilogue) < 2:
        return "unknown"

    # Check last few bytes before ret
    for i in range(len(epilogue) - 2, max(len(epilogue) - 16, -1), -1):
        if i < 0:
            break
        b = epilogue[i]

        # FPU return: fld/fstp patterns (D9/DD prefix with FP operations)
        if b in (0xD9, 0xDD) and i + 1 < len(epilogue):
            next_b = epilogue[i + 1]
            if (b == 0xD9 and next_b in (0xC0, 0xC1, 0xC2, 0xC3)):
                return "float"
            if (b == 0xDD and next_b in (0xC0, 0xC1, 0xC2, 0xC3)):
                return "double"

        # xor eax, eax (33 C0) -> returns 0
        if b == 0x33 and i + 1 < len(epilogue) and epilogue[i + 1] == 0xC0:
            return "int_zero"

        # movss xmm0, ... (F3 0F 10) -> float return (SSE)
        if b == 0xF3 and i + 2 < len(epilogue):
            if epilogue[i + 1] == 0x0F and epilogue[i + 2] == 0x10:
                return "float_sse"

    return "int_or_void"


def _detect_stack_size(prologue, frame_type):
    """Detect stack frame allocation size."""
    if frame_type == "ebp_frame" and len(prologue) >= 6:
        # After push ebp; mov ebp, esp; look for sub esp, N
        if prologue[3] == 0x83 and prologue[4] == 0xEC:
            return prologue[5]
        if prologue[3:5] == b'\x81\xEC' and len(prologue) >= 9:
            return struct.unpack_from('<I', prologue, 5)[0]

    elif frame_type == "fpo_stack" and len(prologue) >= 3:
        if prologue[0] == 0x83 and prologue[1] == 0xEC:
            return prologue[2]
        if prologue[:2] == b'\x81\xEC' and len(prologue) >= 6:
            return struct.unpack_from('<I', prologue, 2)[0]

    return 0


def _build_statistics(abi_results):
    """Build summary statistics from all function analyses."""
    total = len(abi_results)

    frame_counts = Counter(r["frame_type"] for r in abi_results)
    cc_counts = Counter(r["calling_convention"] for r in abi_results)
    return_counts = Counter(r["return_hint"] for r in abi_results)
    param_counts = Counter(r["estimated_params"] for r in abi_results)

    # Per-category breakdown
    by_category = defaultdict(lambda: {
        "total": 0, "frame": Counter(), "cc": Counter(),
    })
    for r in abi_results:
        cat = r["category"]
        by_category[cat]["total"] += 1
        by_category[cat]["frame"][r["frame_type"]] += 1
        by_category[cat]["cc"][r["calling_convention"]] += 1

    # Stack size distribution
    stack_sizes = [r["stack_frame_size"] for r in abi_results if r["stack_frame_size"] > 0]
    stack_dist = {}
    if stack_sizes:
        brackets = [(1, 16), (17, 64), (65, 256), (257, 1024), (1025, 65536)]
        for lo, hi in brackets:
            stack_dist[f"{lo}-{hi}"] = sum(1 for s in stack_sizes if lo <= s <= hi)

    # thiscall detection
    thiscall_funcs = [r for r in abi_results
                      if r["calling_convention"] in ("thiscall", "thiscall_cdecl")]

    return {
        "total_analyzed": total,
        "frame_type": dict(frame_counts),
        "calling_convention": dict(cc_counts),
        "return_hint": dict(return_counts),
        "estimated_params": {str(k): v for k, v in sorted(param_counts.items())},
        "stack_size_distribution": stack_dist,
        "thiscall_count": len(thiscall_funcs),
        "by_category": {
            cat: {
                "total": info["total"],
                "frame": dict(info["frame"]),
                "cc": dict(info["cc"]),
            }
            for cat, info in sorted(by_category.items(), key=lambda x: -x[1]["total"])
            if info["total"] >= 5
        },
    }


def _print_statistics(stats):
    """Print summary to stdout."""
    print("\n" + "=" * 60)
    print("ABI ANALYSIS SUMMARY")
    print("=" * 60)

    print(f"  Total functions analyzed: {stats['total_analyzed']:,}")

    print(f"\n  Frame type:")
    for ft, count in sorted(stats["frame_type"].items(), key=lambda x: -x[1]):
        pct = count * 100 / stats["total_analyzed"]
        print(f"    {ft:20s}: {count:5,}  ({pct:5.1f}%)")

    print(f"\n  Calling convention:")
    for cc, count in sorted(stats["calling_convention"].items(), key=lambda x: -x[1]):
        pct = count * 100 / stats["total_analyzed"]
        print(f"    {cc:20s}: {count:5,}  ({pct:5.1f}%)")

    print(f"\n  Return type hints:")
    for rt, count in sorted(stats["return_hint"].items(), key=lambda x: -x[1]):
        pct = count * 100 / stats["total_analyzed"]
        print(f"    {rt:20s}: {count:5,}  ({pct:5.1f}%)")

    if stats["stack_size_distribution"]:
        print(f"\n  Stack frame sizes:")
        for bracket, count in stats["stack_size_distribution"].items():
            print(f"    {bracket:15s}: {count:5,}")

    print(f"\n  Thiscall functions: {stats['thiscall_count']:,}")

    print(f"\n  By category (top):")
    for cat, info in list(stats["by_category"].items())[:12]:
        ebp = info["frame"].get("ebp_frame", 0)
        fpo = info["total"] - ebp
        tc = info["cc"].get("thiscall", 0) + info["cc"].get("thiscall_cdecl", 0)
        print(f"    {cat:28s}: {info['total']:5,}  "
              f"EBP={ebp:4d}  FPO={fpo:4d}  thiscall={tc:4d}")

    print("=" * 60)


def _va_to_file_offset(va):
    """Convert a virtual address to a file offset."""
    if TEXT_VA_START <= va < TEXT_VA_START + TEXT_VA_SIZE:
        return va - TEXT_VA_START + TEXT_RAW_ADDR
    return None


def _load_binary(path):
    if not os.path.exists(path):
        raise FileNotFoundError(f"File not found: {path}")
    with open(path, "rb") as f:
        return f.read()


def _load_json(path):
    if not os.path.exists(path):
        raise FileNotFoundError(f"JSON file not found: {path}")
    with open(path, "r", encoding="utf-8") as f:
        return json.load(f)


def _write_json(path, data):
    with open(path, "w", encoding="utf-8") as f:
        json.dump(data, f, indent=2, ensure_ascii=False)
