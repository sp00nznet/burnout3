"""
Output writers for the disassembly tool.

Produces:
- JSON databases (functions.json, xrefs.json, strings.json, labels.json, summary.json)
- Human-readable ASM listings (text.asm, per-section .asm files)
"""

import json
import os
from pathlib import Path
from typing import Dict, List, Optional

from .engine import DisasmEngine, Instruction
from .functions import FunctionDetector, Function
from .xrefs import XRefTracker
from .labels import LabelManager, Label
from .loader import BinaryImage, SectionInfo


class OutputWriter:
    """
    Generates all output files from the disassembly analysis.
    """

    def __init__(self, output_dir: str, engine: DisasmEngine,
                 functions: FunctionDetector, xrefs: XRefTracker,
                 labels: LabelManager, image: BinaryImage,
                 strings: List[dict]):
        self.output_dir = Path(output_dir)
        self.engine = engine
        self.functions = functions
        self.xrefs = xrefs
        self.labels = labels
        self.image = image
        self.strings = strings

    def write_all(self, sections_to_disasm: Optional[List[SectionInfo]] = None,
                  verbose: bool = False) -> None:
        """Write all output files."""
        self.output_dir.mkdir(parents=True, exist_ok=True)

        if verbose:
            print(f"  Writing JSON databases to {self.output_dir}/")

        self._write_summary()
        self._write_functions_json()
        self._write_xrefs_json()
        self._write_strings_json()
        self._write_labels_json()

        if sections_to_disasm:
            asm_dir = self.output_dir / "asm"
            asm_dir.mkdir(parents=True, exist_ok=True)

            if verbose:
                print(f"  Writing ASM listings to {asm_dir}/")

            for sec in sections_to_disasm:
                self._write_section_asm(sec, asm_dir, verbose)

    def _write_summary(self) -> None:
        """Write summary.json with statistics."""
        func_summary = self.functions.summary()
        xref_counts = self.xrefs.count_by_type()

        summary = {
            "binary": self.image.filepath,
            "base_address": f"0x{self.image.base_address:08X}",
            "entry_point": f"0x{self.image.entry_point:08X}",
            "total_instructions": self.engine.instruction_count(),
            "total_functions": func_summary["total_functions"],
            "functions_with_prologue": func_summary["with_prologue"],
            "functions_by_method": func_summary["by_detection_method"],
            "functions_by_section": func_summary["by_section"],
            "total_xrefs": self.xrefs.count(),
            "xrefs_by_type": xref_counts,
            "total_labels": self.labels.count(),
            "total_strings": len(self.strings),
            "kernel_imports": len(self.image.kernel_imports),
            "sections_analyzed": [
                {
                    "name": sec.name,
                    "va": f"0x{sec.virtual_addr:08X}",
                    "size": sec.virtual_size,
                    "functions": len(self.functions.get_functions_in_section(sec.name)),
                }
                for sec in self.image.get_code_sections()
            ],
        }

        with open(self.output_dir / "summary.json", 'w') as f:
            json.dump(summary, f, indent=2)

    def _write_functions_json(self) -> None:
        """Write functions.json with the complete function database."""
        funcs = sorted(self.functions.functions.values(), key=lambda f: f.start)
        data = [f.to_dict() for f in funcs]
        with open(self.output_dir / "functions.json", 'w') as f:
            json.dump(data, f, indent=2)

    def _write_xrefs_json(self) -> None:
        """Write xrefs.json with all cross-references."""
        data = self.xrefs.to_list()
        with open(self.output_dir / "xrefs.json", 'w') as f:
            json.dump(data, f, indent=2)

    def _write_strings_json(self) -> None:
        """Write strings.json with string references."""
        data = []
        for s in self.strings:
            entry = {
                "address": f"0x{s['address']:08X}",
                "string": s["string"],
                "length": s["length"],
            }
            # Add xrefs to this string
            refs_to = self.xrefs.get_refs_to(s["address"])
            if refs_to:
                entry["referenced_from"] = [
                    f"0x{r.from_addr:08X}" for r in refs_to
                ]
            data.append(entry)

        with open(self.output_dir / "strings.json", 'w') as f:
            json.dump(data, f, indent=2)

    def _write_labels_json(self) -> None:
        """Write labels.json with all named symbols."""
        data = self.labels.to_list()
        with open(self.output_dir / "labels.json", 'w') as f:
            json.dump(data, f, indent=2)

    def _write_section_asm(self, section: SectionInfo, asm_dir: Path,
                           verbose: bool = False) -> None:
        """Write a human-readable ASM listing for a section."""
        safe_name = section.name.replace('$', '').replace('.', '')
        if not safe_name:
            safe_name = f"section_{section.virtual_addr:08X}"
        filename = f"{safe_name}.asm"

        if verbose:
            print(f"    {filename}...")

        insns = self.engine.get_instructions_in_range(
            section.virtual_addr,
            section.virtual_addr + section.virtual_size
        )

        funcs_in_section = self.functions.get_functions_in_section(section.name)
        func_starts = {f.start for f in funcs_in_section}
        func_ends = {f.end for f in funcs_in_section}
        func_by_start = {f.start: f for f in funcs_in_section}

        with open(asm_dir / filename, 'w') as f:
            # Header
            f.write(f"; ============================================================\n")
            f.write(f"; Section: {section.name}\n")
            f.write(f"; VA: 0x{section.virtual_addr:08X} - "
                    f"0x{section.virtual_addr + section.virtual_size:08X}\n")
            f.write(f"; Size: {section.virtual_size} bytes "
                    f"({section.virtual_size / 1024:.1f} KB)\n")
            f.write(f"; Functions: {len(funcs_in_section)}\n")
            f.write(f"; Instructions: {len(insns)}\n")
            f.write(f"; ============================================================\n\n")

            for insn in insns:
                addr = insn.address

                # Function boundary markers
                if addr in func_starts:
                    func = func_by_start[addr]
                    f.write(f"\n; {'=' * 60}\n")
                    f.write(f"; Function: {func.name}\n")
                    f.write(f"; Start: 0x{func.start:08X}  End: 0x{func.end:08X}  "
                            f"Size: {func.size} bytes\n")
                    f.write(f"; Detection: {func.detection_method} "
                            f"(confidence: {func.confidence:.2f})\n")
                    if func.calls_to:
                        callees = ", ".join(
                            self.labels.get_display_name(a)
                            for a in func.calls_to[:10]
                        )
                        if len(func.calls_to) > 10:
                            callees += f" ... (+{len(func.calls_to) - 10} more)"
                        f.write(f"; Calls: {callees}\n")
                    if func.called_by:
                        callers = ", ".join(
                            self.labels.get_display_name(a)
                            for a in func.called_by[:10]
                        )
                        if len(func.called_by) > 10:
                            callers += f" ... (+{len(func.called_by) - 10} more)"
                        f.write(f"; Called by: {callers}\n")
                    f.write(f"; {'=' * 60}\n")
                    f.write(f"{func.name}:\n")

                # Label for non-function addresses
                elif self.labels.has(addr):
                    label = self.labels.get(addr)
                    f.write(f"\n{label.name}:\n")

                # Xref comments
                refs_to = self.xrefs.get_refs_to(addr)
                if refs_to and addr not in func_starts:
                    ref_strs = []
                    for ref in refs_to[:5]:
                        ref_strs.append(
                            f"0x{ref.from_addr:08X} ({ref.xref_type.value})")
                    if len(refs_to) > 5:
                        ref_strs.append(f"... (+{len(refs_to) - 5} more)")
                    f.write(f"                                        "
                            f"; XREF: {', '.join(ref_strs)}\n")

                # The instruction itself
                # Format: ADDR  BYTES                MNEMONIC  OPERANDS  ; comment
                bytes_str = insn.bytes_hex
                if len(bytes_str) > 20:
                    bytes_str = bytes_str[:20] + ".."

                # Build comment for resolved targets
                comment = ""
                if insn.call_target is not None:
                    target_name = self.labels.get_display_name(insn.call_target)
                    if not target_name.startswith("0x"):
                        comment = f"; -> {target_name}"
                elif insn.is_call and insn.memory_ref is not None:
                    ki = self.image.get_kernel_import_at_thunk(insn.memory_ref)
                    if ki:
                        comment = f"; -> xbox_{ki.name}"
                elif insn.jump_target is not None:
                    target_name = self.labels.get_display_name(insn.jump_target)
                    if not target_name.startswith("0x"):
                        comment = f"; -> {target_name}"

                f.write(
                    f"  0x{addr:08X}  {bytes_str:<22s}  "
                    f"{insn.mnemonic:<8s} {insn.op_str:<30s} {comment}\n"
                )

                # Function end marker
                if addr + insn.size in func_ends:
                    f.write(f"; end of function\n")


def print_stats(engine: DisasmEngine, functions: FunctionDetector,
                xrefs: XRefTracker, labels: LabelManager,
                strings: List[dict], image: BinaryImage) -> None:
    """Print analysis statistics to stdout."""
    summary = functions.summary()
    xref_counts = xrefs.count_by_type()

    print(f"\n{'=' * 60}")
    print(f"  Disassembly Analysis Summary")
    print(f"{'=' * 60}")
    print(f"  Binary: {image.filepath}")
    print(f"  Base: 0x{image.base_address:08X}  "
          f"Entry: 0x{image.entry_point:08X}")
    print(f"\n  Instructions:     {engine.instruction_count():>10,d}")
    print(f"  Functions:        {summary['total_functions']:>10,d}")
    print(f"    with prologue:  {summary['with_prologue']:>10,d}")
    print(f"  Cross-references: {xrefs.count():>10,d}")
    print(f"  Labels:           {labels.count():>10,d}")
    print(f"  Strings:          {len(strings):>10,d}")
    print(f"  Kernel imports:   {len(image.kernel_imports):>10,d}")

    print(f"\n  Detection methods:")
    for method, count in sorted(summary["by_detection_method"].items()):
        print(f"    {method:<20s} {count:>8,d}")

    print(f"\n  Functions by section:")
    for sec, count in sorted(summary["by_section"].items()):
        print(f"    {sec:<20s} {count:>8,d}")

    print(f"\n  Cross-references by type:")
    for xtype, count in sorted(xref_counts.items()):
        print(f"    {xtype:<20s} {count:>8,d}")

    # Kernel import usage summary
    kernel_calls = xrefs.all_kernel_calls()
    if kernel_calls:
        print(f"\n  Top kernel imports (by call count):")
        sorted_ki = sorted(kernel_calls.items(),
                           key=lambda x: len(x[1]), reverse=True)
        for thunk_addr, callers in sorted_ki[:15]:
            ki = image.get_kernel_import_at_thunk(thunk_addr)
            name = ki.name if ki else f"0x{thunk_addr:08X}"
            print(f"    {name:<40s} {len(callers):>6d} calls")
        if len(sorted_ki) > 15:
            print(f"    ... ({len(sorted_ki) - 15} more)")

    print(f"\n{'=' * 60}")
