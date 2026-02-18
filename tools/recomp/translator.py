"""
Function-level x86 → C translator.

For each function:
1. Read raw bytes from XBE
2. Disassemble with Capstone
3. Build basic blocks
4. Lift each block to C statements
5. Generate a complete C function

Produces compilable C code using recomp_types.h macros.
"""

import json
import os

from .config import va_to_file_offset, is_code_address, TEXT_VA_START, TEXT_VA_END
from .disasm import Disassembler
from .lifter import Lifter, lift_basic_block


class FunctionTranslator:
    """Translates individual x86 functions to C source code."""

    def __init__(self, xbe_data, func_db, label_db=None, classification_db=None,
                 abi_db=None):
        """
        xbe_data: bytes - raw XBE file contents
        func_db: dict - addr → function info from functions.json
        label_db: dict - addr → name from labels.json
        classification_db: dict - addr → classification from identified_functions.json
        abi_db: dict - addr → ABI info from abi_functions.json
        """
        self.xbe_data = xbe_data
        self.func_db = func_db
        self.label_db = label_db or {}
        self.classification_db = classification_db or {}
        self.abi_db = abi_db or {}
        self.disasm = Disassembler()
        self.lifter = Lifter(func_db=func_db, label_db=label_db, abi_db=abi_db)

    def _read_func_bytes(self, start_va, end_va):
        """Read raw bytes for a function from the XBE."""
        offset = va_to_file_offset(start_va)
        if offset is None:
            return None
        size = end_va - start_va
        if offset + size > len(self.xbe_data):
            return None
        return self.xbe_data[offset:offset + size]

    def _determine_calling_convention(self, func_info):
        """Guess calling convention from function properties."""
        name = func_info.get("name", "")
        # thiscall methods have ecx = this
        if "thiscall" in name or func_info.get("calling_convention") == "thiscall":
            return "thiscall"
        return "cdecl"

    def _func_has_prologue(self, instructions):
        """Check if function starts with push ebp; mov ebp, esp."""
        if len(instructions) < 2:
            return False
        return (instructions[0].mnemonic == "push" and
                instructions[0].op_str == "ebp" and
                instructions[1].mnemonic == "mov" and
                instructions[1].op_str == "ebp, esp")

    def translate_function(self, func_addr, func_info):
        """
        Translate a single function to C code.
        Returns a string of C source code, or None on failure.
        """
        start = func_addr
        end = func_info.get("end")
        if not end:
            end = start + func_info.get("size", 0)
        if end <= start:
            return None

        name = func_info.get("name", f"sub_{start:08X}")
        size = end - start

        # Read bytes from XBE
        raw_bytes = self._read_func_bytes(start, end)
        if not raw_bytes:
            return None

        # Set function bounds for the lifter
        self.lifter.func_start = start
        self.lifter.func_end = end

        # Disassemble
        instructions = self.disasm.disassemble_function(raw_bytes, start, end)
        if not instructions:
            return None

        # Build basic blocks
        blocks = self.disasm.build_basic_blocks(instructions, start, end)
        if not blocks:
            return None

        # Get classification and ABI info
        cls_info = self.classification_db.get(start, {})
        category = cls_info.get("category", "unknown")
        module = cls_info.get("module", "")
        source_file = cls_info.get("source_file", "")
        abi_info = self.abi_db.get(start, {})

        # ABI-derived info
        cc = abi_info.get("calling_convention", "cdecl")
        num_params = abi_info.get("estimated_params", 0)
        return_hint = abi_info.get("return_hint", "int_or_void")
        frame_type = abi_info.get("frame_type", "fpo_leaf")
        stack_frame_size = abi_info.get("stack_frame_size", 0)

        # Determine which registers are used
        used_regs = self._find_used_registers(instructions)
        used_xmm = self._find_used_xmm(instructions)
        has_prologue = self._func_has_prologue(instructions)
        has_fpu = any(insn.mnemonic.startswith("f") for insn in instructions)

        # Ensure esp is declared if function uses stack operations
        has_stack_ops = any(insn.mnemonic in ("push", "pop", "leave", "call")
                           or (insn.mnemonic == "ret" and has_prologue)
                           for insn in instructions)
        if has_stack_ops:
            used_regs.add("esp")

        # Ensure ecx is declared if function calls thiscall functions
        for insn in instructions:
            target = insn.call_target or insn.jump_target
            if target and target in self.abi_db:
                callee_cc = self.abi_db[target].get("calling_convention", "")
                if callee_cc in ("thiscall", "thiscall_cdecl"):
                    used_regs.add("ecx")
                    break

        # Ensure ebp declared if function uses 'leave' (implicit ebp)
        if any(insn.mnemonic == "leave" for insn in instructions):
            used_regs.add("ebp")

        # Ensure edx/eax declared for implicit-operand instructions
        for insn in instructions:
            if insn.mnemonic in ("cdq", "div", "idiv", "mul"):
                used_regs.add("edx")
                used_regs.add("eax")

        # Build call targets list
        call_targets = set()
        for insn in instructions:
            if insn.call_target and is_code_address(insn.call_target):
                call_targets.add(insn.call_target)

        # Determine return type
        if return_hint == "float_sse" or return_hint == "float":
            ret_type = "float"
        elif return_hint == "int_zero":
            ret_type = "int"
        elif num_params == 0 and return_hint == "int_or_void":
            ret_type = "void"
        else:
            ret_type = "uint32_t"

        # Build parameter list
        is_thiscall = cc in ("thiscall", "thiscall_cdecl")
        params = []
        if is_thiscall:
            params.append("void *this_ptr")
        for i in range(num_params):
            params.append(f"uint32_t a{i+1}")
        param_str = ", ".join(params) if params else "void"

        # Generate C code
        lines = []

        # Header comment
        lines.append(f"/**")
        lines.append(f" * {name}")
        lines.append(f" * Original: 0x{start:08X} - 0x{end:08X} ({size} bytes, {len(instructions)} insns)")
        if category != "unknown":
            lines.append(f" * Category: {category}")
        if source_file:
            lines.append(f" * Source: {source_file}")
        lines.append(f" * CC: {cc}, {num_params} params, returns {return_hint}")
        if frame_type == "ebp_frame":
            lines.append(f" * Frame: EBP-based ({stack_frame_size} bytes locals)")
        else:
            lines.append(f" * Frame: {frame_type}")
        lines.append(f" */")

        # Function signature
        lines.append(f"{ret_type} {name}({param_str})")
        lines.append(f"{{")

        # Register declarations (exclude params)
        reg_decls = []
        for reg in ["eax", "ebx", "ecx", "edx", "esi", "edi", "ebp", "esp"]:
            if reg in used_regs:
                reg_decls.append(reg)
        if reg_decls:
            lines.append(f"    uint32_t {', '.join(reg_decls)};")

        # Add _flags variable if function has conditional instructions
        has_conditionals = any(
            insn.is_cond_jump or insn.mnemonic.startswith("set")
            or insn.mnemonic.startswith("cmov")
            for insn in instructions)
        if has_conditionals:
            lines.append(f"    int _flags = 0; /* fallback flag var */")

        # Add _cf for carry-dependent instructions (sbb, adc)
        has_carry = any(insn.mnemonic in ("sbb", "adc")
                        for insn in instructions)
        if has_carry:
            lines.append(f"    int _cf = 0; /* carry flag */")

        # Add _fpu_cmp for FPU compare instructions (both old and new style)
        has_fpu_cmp = any(insn.mnemonic in ("fcompi", "fcomip", "fucomi",
                                             "fucompi", "fucomip", "fcomi",
                                             "fcom", "fcomp", "fcompp",
                                             "fucom", "fucomp", "fucompp")
                          for insn in instructions)
        if has_fpu_cmp:
            lines.append(f"    int _fpu_cmp = 0; /* FPU compare result: -1/0/1 */")

        # Map parameters to registers/stack locations
        if is_thiscall:
            lines.append(f"    ecx = (uint32_t)(uintptr_t)this_ptr;")
        if num_params > 0:
            lines.append(f"    /* Parameters: {', '.join(f'a{i+1}' for i in range(num_params))} */")

        # SSE/MMX register declarations
        if used_xmm:
            xmm_regs = sorted([r for r in used_xmm if r.startswith("xmm")])
            mmx_regs = sorted([r for r in used_xmm if r.startswith("mm")
                               and not r.startswith("xmm")])
            if xmm_regs:
                lines.append(f"    float {', '.join(xmm_regs)};")
            if mmx_regs:
                lines.append(f"    uint64_t {', '.join(mmx_regs)};")

        # FPU stack (simplified)
        if has_fpu:
            lines.append(f"    double _fp_stack[8];")
            lines.append(f"    int _fp_top = 0;")
            lines.append(f"    #define fp_push(v) (_fp_stack[--_fp_top & 7] = (v))")
            lines.append(f"    #define fp_pop() (_fp_top++)")
            lines.append(f"    #define fp_popp() (fp_pop())")
            lines.append(f"    #define fp_top() _fp_stack[_fp_top & 7]")
            lines.append(f"    #define fp_st1() _fp_stack[(_fp_top + 1) & 7]")

        lines.append(f"")

        # Generate code for each basic block
        # Create a set of addresses that need labels
        label_addrs = set()
        for bb in blocks:
            for succ in bb.successors:
                label_addrs.add(succ)
        # Also add any jump targets within the function
        for insn in instructions:
            if insn.jump_target and start <= insn.jump_target < end:
                label_addrs.add(insn.jump_target)

        flag_state = None
        for bb in blocks:
            # Emit label if this block is a branch target
            if bb.start in label_addrs or bb.start == start:
                lines.append(f"loc_{bb.start:08X}:")

            # Propagate flag state from previous block (fallthrough path).
            # This handles patterns like: test eax,eax / ja X / jb Y
            # where jb uses the same flags as ja from the preceding block.
            stmts, flag_state = lift_basic_block(
                self.lifter, bb, flag_state=flag_state)
            for stmt in stmts:
                lines.append(f"    {stmt}")

            lines.append(f"")

        # Validate: comment out goto targets that reference missing labels
        # (dead code after unconditional jumps may reference non-existent labels)
        import re
        defined_labels = set()
        goto_lines = []
        for idx, line in enumerate(lines):
            lbl_match = re.match(r'^(loc_[0-9A-Fa-f]+):', line)
            if lbl_match:
                defined_labels.add(lbl_match.group(1))
            goto_match = re.search(r'goto (loc_[0-9A-Fa-f]+);', line)
            if goto_match:
                goto_lines.append((idx, goto_match.group(1)))
        for idx, target in goto_lines:
            if target not in defined_labels:
                lines[idx] = lines[idx].replace(
                    f"goto {target};",
                    f"(void)0; /* goto {target} - dead code, label not in function */")

        # Undefine FPU macros
        if has_fpu:
            lines.append(f"    #undef fp_push")
            lines.append(f"    #undef fp_pop")
            lines.append(f"    #undef fp_popp")
            lines.append(f"    #undef fp_top")
            lines.append(f"    #undef fp_st1")

        lines.append(f"}}")
        lines.append(f"")

        return "\n".join(lines)

    def _find_used_registers(self, instructions):
        """Find which 32-bit registers are referenced by any instruction."""
        regs = set()
        reg_map = {
            "eax": "eax", "ax": "eax", "al": "eax", "ah": "eax",
            "ebx": "ebx", "bx": "ebx", "bl": "ebx", "bh": "ebx",
            "ecx": "ecx", "cx": "ecx", "cl": "ecx", "ch": "ecx",
            "edx": "edx", "dx": "edx", "dl": "edx", "dh": "edx",
            "esi": "esi", "si": "esi",
            "edi": "edi", "di": "edi",
            "ebp": "ebp", "bp": "ebp",
            "esp": "esp", "sp": "esp",
        }
        for insn in instructions:
            for op in insn.operands:
                if op.type == "reg" and op.reg in reg_map:
                    regs.add(reg_map[op.reg])
                elif op.type == "mem":
                    if op.mem_base and op.mem_base in reg_map:
                        regs.add(reg_map[op.mem_base])
                    if op.mem_index and op.mem_index in reg_map:
                        regs.add(reg_map[op.mem_index])
        return regs

    def _find_used_xmm(self, instructions):
        """Find which XMM and MMX registers are used."""
        regs = set()
        for insn in instructions:
            for op in insn.operands:
                if op.type == "reg" and op.reg:
                    if op.reg.startswith("xmm") or op.reg.startswith("mm"):
                        regs.add(op.reg)
        return regs


class BatchTranslator:
    """Translates multiple functions and writes C source files."""

    def __init__(self, xbe_path, func_json_path, labels_json_path=None,
                 identified_json_path=None, abi_json_path=None,
                 output_dir=None):
        self.xbe_path = xbe_path
        self.output_dir = output_dir or os.path.join(
            os.path.dirname(__file__), "output")

        # Load XBE
        with open(xbe_path, "rb") as f:
            self.xbe_data = f.read()

        # Load function database
        with open(func_json_path, "r") as f:
            func_list = json.load(f)

        self.func_db = {}
        for func in func_list:
            addr = int(func["start"], 16)
            func["_addr"] = addr
            if "end" in func:
                func["end"] = int(func["end"], 16)
            self.func_db[addr] = func

        # Load labels
        self.label_db = {}
        if labels_json_path and os.path.exists(labels_json_path):
            with open(labels_json_path, "r") as f:
                labels = json.load(f)
            for lbl in labels:
                addr = int(lbl["address"], 16)
                self.label_db[addr] = lbl["name"]

        # Load classifications
        self.classification_db = {}
        if identified_json_path and os.path.exists(identified_json_path):
            with open(identified_json_path, "r") as f:
                identified = json.load(f)
            for entry in identified:
                addr = int(entry["start"], 16)
                self.classification_db[addr] = entry

        # Load ABI data
        self.abi_db = {}
        if abi_json_path and os.path.exists(abi_json_path):
            with open(abi_json_path, "r") as f:
                abi_list = json.load(f)
            for entry in abi_list:
                addr = int(entry["address"], 16)
                self.abi_db[addr] = entry

        # Create translator
        self.translator = FunctionTranslator(
            self.xbe_data, self.func_db, self.label_db,
            self.classification_db, self.abi_db)

    def get_functions_by_category(self, categories=None, exclude_categories=None):
        """
        Get function addresses filtered by category.
        Returns list of (addr, func_info) tuples.
        """
        result = []
        for addr, func_info in sorted(self.func_db.items()):
            cls_info = self.classification_db.get(addr, {})
            cat = cls_info.get("category", "unknown")

            if categories and cat not in categories:
                continue
            if exclude_categories and cat in exclude_categories:
                continue

            result.append((addr, func_info))
        return result

    def _make_declaration(self, addr, name):
        """Generate a function declaration string using ABI data."""
        abi_info = self.abi_db.get(addr, {})
        cc = abi_info.get("calling_convention", "cdecl")
        num_params = abi_info.get("estimated_params", 0)
        return_hint = abi_info.get("return_hint", "int_or_void")

        if return_hint in ("float_sse", "float"):
            ret_type = "float"
        elif return_hint == "int_zero":
            ret_type = "int"
        elif num_params == 0 and return_hint == "int_or_void":
            ret_type = "void"
        else:
            ret_type = "uint32_t"

        is_thiscall = cc in ("thiscall", "thiscall_cdecl")
        params = []
        if is_thiscall:
            params.append("void *this_ptr")
        for i in range(num_params):
            params.append(f"uint32_t a{i+1}")
        param_str = ", ".join(params) if params else "void"

        return f"{ret_type} {name}({param_str})"

    def translate_single(self, addr):
        """Translate a single function by address. Returns C code string."""
        func_info = self.func_db.get(addr)
        if not func_info:
            return None
        return self.translator.translate_function(addr, func_info)

    def translate_batch(self, func_list, output_file=None, max_funcs=None,
                        verbose=False):
        """
        Translate a batch of functions.

        func_list: list of (addr, func_info) tuples
        output_file: path to write combined C output
        max_funcs: limit number of functions
        verbose: print progress

        Returns dict with statistics.
        """
        os.makedirs(self.output_dir, exist_ok=True)

        if max_funcs:
            func_list = func_list[:max_funcs]

        stats = {
            "total": len(func_list),
            "translated": 0,
            "failed": 0,
            "total_lines": 0,
            "total_insns": 0,
        }

        c_chunks = []
        c_chunks.append("/**")
        c_chunks.append(" * Burnout 3: Takedown - Mechanically Translated Game Code")
        c_chunks.append(f" * Generated by tools/recomp from original Xbox x86 code.")
        c_chunks.append(f" * Functions: {len(func_list)}")
        c_chunks.append(" */")
        c_chunks.append("")
        c_chunks.append('#include "recomp_types.h"')
        c_chunks.append('#include <math.h>')
        c_chunks.append("")
        c_chunks.append("/* Forward declarations */")

        # Forward declarations
        for addr, func_info in func_list:
            name = func_info.get("name", f"sub_{addr:08X}")
            decl = self._make_declaration(addr, name)
            c_chunks.append(f"{decl};")
        c_chunks.append("")
        c_chunks.append("/* ═══════════════════════════════════════════════════ */")
        c_chunks.append("")

        # Translate each function
        for i, (addr, func_info) in enumerate(func_list):
            name = func_info.get("name", f"sub_{addr:08X}")
            if verbose and (i % 100 == 0 or i == len(func_list) - 1):
                print(f"  [{i+1}/{len(func_list)}] Translating {name} at 0x{addr:08X}...")

            code = self.translator.translate_function(addr, func_info)
            if code:
                c_chunks.append(code)
                stats["translated"] += 1
                stats["total_lines"] += code.count("\n")

                # Count instructions
                num_insns = func_info.get("num_instructions", 0)
                stats["total_insns"] += num_insns
            else:
                c_chunks.append(f"/* FAILED to translate {name} at 0x{addr:08X} */")
                c_chunks.append(f"void {name}(void) {{ /* translation failed */ }}")
                c_chunks.append("")
                stats["failed"] += 1

        # Write output
        if output_file is None:
            output_file = os.path.join(self.output_dir, "recompiled.c")

        output_text = "\n".join(c_chunks)
        with open(output_file, "w", encoding="utf-8") as f:
            f.write(output_text)

        stats["output_file"] = output_file
        stats["output_size"] = len(output_text)

        return stats

    def translate_by_category(self, categories, output_prefix=None,
                              max_per_file=500, verbose=False):
        """
        Translate functions grouped by category, one file per category.
        Returns dict with per-category stats.
        """
        os.makedirs(self.output_dir, exist_ok=True)
        all_stats = {}

        for cat in categories:
            funcs = self.get_functions_by_category(categories={cat})
            if not funcs:
                continue

            prefix = output_prefix or cat
            out_file = os.path.join(self.output_dir, f"{prefix}.c")

            if verbose:
                print(f"\nCategory: {cat} ({len(funcs)} functions)")

            stats = self.translate_batch(
                funcs, output_file=out_file,
                max_funcs=max_per_file, verbose=verbose)
            all_stats[cat] = stats

        return all_stats

    def translate_batch_split(self, func_list, output_dir, chunk_size=1000,
                              header_name="recomp_funcs.h",
                              prefix="recomp", verbose=False):
        """
        Translate functions into multiple .c files + a shared header.

        Generates:
          output_dir/recomp_funcs.h       - forward declarations for all functions
          output_dir/recomp_0000.c        - chunk 0
          output_dir/recomp_0001.c        - chunk 1
          ...
          output_dir/recomp_dispatch.c    - address -> function pointer table

        Returns dict with stats and list of generated files.
        """
        import sys

        os.makedirs(output_dir, exist_ok=True)

        # Translate all functions first, collecting results
        translations = []
        stats = {
            "total": len(func_list),
            "translated": 0,
            "failed": 0,
            "total_lines": 0,
        }

        for i, (addr, func_info) in enumerate(func_list):
            name = func_info.get("name", f"sub_{addr:08X}")
            if verbose and (i % 500 == 0 or i == len(func_list) - 1):
                print(f"  [{i+1}/{len(func_list)}] Translating {name}...",
                      file=sys.stderr)

            code = self.translator.translate_function(addr, func_info)
            if code:
                translations.append((addr, name, code))
                stats["translated"] += 1
                stats["total_lines"] += code.count("\n")
            else:
                # Stub for failed translations
                stub = f"/* FAILED: {name} at 0x{addr:08X} */\n"
                stub += f"void {name}(void) {{ /* translation failed */ }}\n"
                translations.append((addr, name, stub))
                stats["failed"] += 1

        # Generate header with all forward declarations
        header_path = os.path.join(output_dir, header_name)
        header_lines = [
            "/**",
            " * Burnout 3: Takedown - Recompiled Function Declarations",
            f" * {stats['translated']} functions, auto-generated by tools/recomp",
            " */",
            "",
            "#ifndef RECOMP_FUNCS_H",
            "#define RECOMP_FUNCS_H",
            "",
            '#include "recomp_types.h"',
            "",
        ]
        for addr, name, _ in translations:
            decl = self._make_declaration(addr, name)
            header_lines.append(f"{decl};")
        header_lines.extend(["", "#endif /* RECOMP_FUNCS_H */", ""])

        with open(header_path, "w", encoding="utf-8") as f:
            f.write("\n".join(header_lines))

        # Split translations into chunks and write .c files
        generated_files = [header_path]
        chunks = [translations[i:i+chunk_size]
                  for i in range(0, len(translations), chunk_size)]

        for ci, chunk in enumerate(chunks):
            c_path = os.path.join(output_dir, f"{prefix}_{ci:04d}.c")
            c_lines = [
                "/**",
                f" * Burnout 3 - Recompiled code chunk {ci}",
                f" * Functions: {len(chunk)} "
                f"(0x{chunk[0][0]:08X} - 0x{chunk[-1][0]:08X})",
                " */",
                "",
                f'#include "{header_name}"',
                '#include <math.h>',
                "",
            ]
            for addr, name, code in chunk:
                c_lines.append(code)

            with open(c_path, "w", encoding="utf-8") as f:
                f.write("\n".join(c_lines))
            generated_files.append(c_path)

            if verbose:
                print(f"  Wrote {c_path} ({len(chunk)} functions)",
                      file=sys.stderr)

        # Generate dispatch table
        dispatch_path = os.path.join(output_dir, f"{prefix}_dispatch.c")
        self._write_dispatch_table(translations, dispatch_path, header_name)
        generated_files.append(dispatch_path)

        stats["files"] = generated_files
        stats["num_chunks"] = len(chunks)
        stats["chunk_size"] = chunk_size
        return stats

    def _write_dispatch_table(self, translations, output_path, header_name):
        """
        Generate a dispatch table mapping Xbox VA -> function pointer.

        Uses a sorted array + binary search for O(log n) lookup.
        """
        lines = [
            "/**",
            " * Burnout 3 - Recompiled Function Dispatch Table",
            f" * Maps {len(translations)} Xbox VAs to translated function pointers.",
            " * Auto-generated by tools/recomp",
            " */",
            "",
            f'#include "{header_name}"',
            '#include <stddef.h>',
            "",
            "/* Generic function pointer type */",
            "typedef void (*recomp_func_t)(void);",
            "",
            "typedef struct {",
            "    uint32_t xbox_va;",
            "    recomp_func_t func;",
            "} recomp_entry_t;",
            "",
            f"static const recomp_entry_t g_recomp_table[] = {{",
        ]

        for addr, name, _ in translations:
            lines.append(f"    {{ 0x{addr:08X}u, (recomp_func_t){name} }},")

        lines.extend([
            "};",
            "",
            f"static const size_t g_recomp_table_size = "
            f"{len(translations)};",
            "",
            "/* Binary search for a function by Xbox VA */",
            "recomp_func_t recomp_lookup(uint32_t xbox_va)",
            "{",
            "    size_t lo = 0, hi = g_recomp_table_size;",
            "    while (lo < hi) {",
            "        size_t mid = lo + (hi - lo) / 2;",
            "        if (g_recomp_table[mid].xbox_va < xbox_va)",
            "            lo = mid + 1;",
            "        else if (g_recomp_table[mid].xbox_va > xbox_va)",
            "            hi = mid;",
            "        else",
            "            return g_recomp_table[mid].func;",
            "    }",
            "    return NULL;",
            "}",
            "",
            "/* Get the number of registered functions */",
            "size_t recomp_get_count(void)",
            "{",
            "    return g_recomp_table_size;",
            "}",
            "",
            "/* Call all registered functions (for bulk testing) */",
            "size_t recomp_call_all(void)",
            "{",
            "    size_t i;",
            "    for (i = 0; i < g_recomp_table_size; i++) {",
            "        g_recomp_table[i].func();",
            "    }",
            "    return g_recomp_table_size;",
            "}",
            "",
        ])

        with open(output_path, "w", encoding="utf-8") as f:
            f.write("\n".join(lines))
