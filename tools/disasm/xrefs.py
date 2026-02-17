"""
Cross-reference tracking for the disassembler.

Tracks all references between code and data: calls, jumps,
data references, and kernel import usage.
"""

from dataclasses import dataclass, field
from enum import Enum
from typing import Dict, List, Optional, Set, Tuple

from .engine import DisasmEngine, Instruction
from .loader import BinaryImage


class XRefType(Enum):
    CALL = "call"               # Direct function call
    JUMP = "jump"               # Unconditional jump
    COND_JUMP = "cond_jump"     # Conditional jump
    DATA_READ = "data_read"     # Memory read reference
    KERNEL_CALL = "kernel_call" # Call through kernel thunk


@dataclass
class XRef:
    """A single cross-reference."""
    from_addr: int
    to_addr: int
    xref_type: XRefType
    # For kernel calls, the import name
    kernel_name: Optional[str] = None

    def to_dict(self) -> dict:
        d = {
            "from": f"0x{self.from_addr:08X}",
            "to": f"0x{self.to_addr:08X}",
            "type": self.xref_type.value,
        }
        if self.kernel_name:
            d["kernel_name"] = self.kernel_name
        return d


class XRefTracker:
    """
    Collects and indexes cross-references from disassembled instructions.
    """

    def __init__(self):
        # Forward refs: from_addr -> list of XRef
        self._from: Dict[int, List[XRef]] = {}
        # Backward refs: to_addr -> list of XRef
        self._to: Dict[int, List[XRef]] = {}
        # Kernel import call sites: thunk_addr -> list of calling addresses
        self._kernel_calls: Dict[int, List[int]] = {}

    def add(self, xref: XRef) -> None:
        """Add a cross-reference."""
        self._from.setdefault(xref.from_addr, []).append(xref)
        self._to.setdefault(xref.to_addr, []).append(xref)

    def get_refs_from(self, addr: int) -> List[XRef]:
        """Get all references originating from an address."""
        return self._from.get(addr, [])

    def get_refs_to(self, addr: int) -> List[XRef]:
        """Get all references pointing to an address."""
        return self._to.get(addr, [])

    def get_callers(self, func_addr: int) -> List[int]:
        """Get all addresses that call a function."""
        refs = self._to.get(func_addr, [])
        return [r.from_addr for r in refs if r.xref_type == XRefType.CALL]

    def get_callees(self, from_addr: int) -> List[int]:
        """Get all call targets from an address."""
        refs = self._from.get(from_addr, [])
        return [r.to_addr for r in refs if r.xref_type == XRefType.CALL]

    def get_kernel_call_sites(self, thunk_addr: int) -> List[int]:
        """Get all call sites for a kernel import thunk."""
        return self._kernel_calls.get(thunk_addr, [])

    def all_kernel_calls(self) -> Dict[int, List[int]]:
        """Return all kernel import call mappings."""
        return dict(self._kernel_calls)

    def count(self) -> int:
        return sum(len(refs) for refs in self._from.values())

    def count_by_type(self) -> Dict[str, int]:
        counts: Dict[str, int] = {}
        for refs in self._from.values():
            for r in refs:
                key = r.xref_type.value
                counts[key] = counts.get(key, 0) + 1
        return counts

    def to_list(self) -> List[dict]:
        """Export all xrefs as a flat list of dicts, sorted by from_addr."""
        result = []
        for addr in sorted(self._from.keys()):
            for xref in self._from[addr]:
                result.append(xref.to_dict())
        return result


def build_xrefs(engine: DisasmEngine, image: BinaryImage) -> XRefTracker:
    """
    Build the complete cross-reference database from decoded instructions.

    Scans all instructions and creates XRef entries for:
    - Direct calls (call rel32)
    - Indirect calls through kernel thunks (call [thunk_addr])
    - Direct jumps (jmp rel32)
    - Conditional jumps (jcc rel8/rel32)
    - Data memory references (mov reg, [abs_addr] etc.)

    Args:
        engine: The disassembly engine with decoded instructions.
        image: The binary image for kernel import resolution.

    Returns:
        A fully populated XRefTracker.
    """
    tracker = XRefTracker()

    for insn in engine.instructions.values():
        # Direct calls
        if insn.is_call and insn.call_target is not None:
            tracker.add(XRef(
                from_addr=insn.address,
                to_addr=insn.call_target,
                xref_type=XRefType.CALL,
            ))

        # Indirect calls through memory (kernel thunks)
        if insn.is_call and insn.memory_ref is not None:
            ki = image.get_kernel_import_at_thunk(insn.memory_ref)
            if ki is not None:
                xref = XRef(
                    from_addr=insn.address,
                    to_addr=insn.memory_ref,
                    xref_type=XRefType.KERNEL_CALL,
                    kernel_name=ki.name,
                )
                tracker.add(xref)
                tracker._kernel_calls.setdefault(
                    insn.memory_ref, []).append(insn.address)
            else:
                # Indirect call to non-kernel memory address
                tracker.add(XRef(
                    from_addr=insn.address,
                    to_addr=insn.memory_ref,
                    xref_type=XRefType.CALL,
                ))

        # Indirect jumps through memory (e.g., switch tables, thunks)
        if (insn.is_jump or insn.is_cond_jump) and insn.memory_ref is not None:
            ki = image.get_kernel_import_at_thunk(insn.memory_ref)
            if ki is not None:
                xref = XRef(
                    from_addr=insn.address,
                    to_addr=insn.memory_ref,
                    xref_type=XRefType.KERNEL_CALL,
                    kernel_name=ki.name,
                )
                tracker.add(xref)
                tracker._kernel_calls.setdefault(
                    insn.memory_ref, []).append(insn.address)

        # Direct jumps
        if insn.is_jump and insn.jump_target is not None:
            tracker.add(XRef(
                from_addr=insn.address,
                to_addr=insn.jump_target,
                xref_type=XRefType.JUMP,
            ))

        # Conditional jumps
        if insn.is_cond_jump and insn.jump_target is not None:
            tracker.add(XRef(
                from_addr=insn.address,
                to_addr=insn.jump_target,
                xref_type=XRefType.COND_JUMP,
            ))

        # Data references (non-branch memory operands)
        if not (insn.is_call or insn.is_branch) and insn.memory_ref is not None:
            tracker.add(XRef(
                from_addr=insn.address,
                to_addr=insn.memory_ref,
                xref_type=XRefType.DATA_READ,
            ))

    return tracker
