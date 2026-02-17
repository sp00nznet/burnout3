"""
Disassembly engine using Capstone.

Provides linear sweep and recursive descent disassembly of x86-32 code,
with instruction classification and operand analysis.
"""

import struct
from dataclasses import dataclass, field
from typing import Dict, List, Optional, Set, Tuple

from capstone import Cs, CS_ARCH_X86, CS_MODE_32, CsInsn
from capstone import CS_OP_IMM, CS_OP_MEM

from . import config
from .loader import BinaryImage, SectionInfo


@dataclass
class Instruction:
    """A decoded instruction with metadata."""
    address: int
    size: int
    mnemonic: str
    op_str: str
    bytes_hex: str

    # Classification
    is_call: bool = False
    is_ret: bool = False
    is_jump: bool = False
    is_cond_jump: bool = False
    is_nop: bool = False

    # Resolved targets
    call_target: Optional[int] = None     # For direct calls
    jump_target: Optional[int] = None     # For direct jumps
    memory_ref: Optional[int] = None      # For [addr] references

    @property
    def is_branch(self) -> bool:
        return self.is_jump or self.is_cond_jump

    @property
    def is_terminator(self) -> bool:
        return self.is_ret or self.is_jump  # Unconditional terminator

    @property
    def end_address(self) -> int:
        return self.address + self.size

    def to_dict(self) -> dict:
        d = {
            "address": f"0x{self.address:08X}",
            "size": self.size,
            "mnemonic": self.mnemonic,
            "op_str": self.op_str,
            "bytes": self.bytes_hex,
        }
        if self.call_target is not None:
            d["call_target"] = f"0x{self.call_target:08X}"
        if self.jump_target is not None:
            d["jump_target"] = f"0x{self.jump_target:08X}"
        if self.memory_ref is not None:
            d["memory_ref"] = f"0x{self.memory_ref:08X}"
        return d


class DisasmEngine:
    """
    x86-32 disassembly engine backed by Capstone.

    Supports both linear sweep (complete coverage) and recursive descent
    (reachability validation) modes.
    """

    def __init__(self, image: BinaryImage):
        self.image = image
        self._cs = Cs(CS_ARCH_X86, CS_MODE_32)
        self._cs.detail = True

        # Instruction cache: address -> Instruction
        self.instructions: Dict[int, Instruction] = {}

        # Sorted address list (built lazily for range queries)
        self._sorted_addrs: Optional[List[int]] = None

    def _classify_instruction(self, cs_insn: CsInsn) -> Instruction:
        """Convert a Capstone instruction to our Instruction type."""
        mnemonic = cs_insn.mnemonic
        insn = Instruction(
            address=cs_insn.address,
            size=cs_insn.size,
            mnemonic=mnemonic,
            op_str=cs_insn.op_str,
            bytes_hex=cs_insn.bytes.hex(),
        )

        # Classify by mnemonic
        insn.is_call = mnemonic in config.CALL_MNEMONICS
        insn.is_ret = mnemonic in config.RET_MNEMONICS
        insn.is_jump = mnemonic in config.JMP_MNEMONICS
        insn.is_cond_jump = mnemonic in config.COND_JMP_MNEMONICS
        insn.is_nop = mnemonic in config.NOP_MNEMONICS

        # Resolve operand targets using Capstone detail
        try:
            operands = cs_insn.operands
        except Exception:
            operands = []

        if operands:
            op = operands[0]

            if insn.is_call:
                if op.type == CS_OP_IMM:
                    insn.call_target = op.imm & 0xFFFFFFFF
                elif op.type == CS_OP_MEM and op.mem.base == 0 and op.mem.index == 0:
                    insn.memory_ref = op.mem.disp & 0xFFFFFFFF

            elif insn.is_jump or insn.is_cond_jump:
                if op.type == CS_OP_IMM:
                    insn.jump_target = op.imm & 0xFFFFFFFF
                elif op.type == CS_OP_MEM and op.mem.base == 0 and op.mem.index == 0:
                    insn.memory_ref = op.mem.disp & 0xFFFFFFFF

            # Check for memory references in non-branch instructions
            if not (insn.is_call or insn.is_branch) and insn.memory_ref is None:
                for operand in operands:
                    if (operand.type == CS_OP_MEM and
                            operand.mem.base == 0 and operand.mem.index == 0):
                        addr = operand.mem.disp & 0xFFFFFFFF
                        if self.image.base_address <= addr < (
                                self.image.base_address + self.image.image_size):
                            insn.memory_ref = addr
                            break

        return insn

    def linear_sweep(self, section: SectionInfo,
                     progress_callback=None) -> int:
        """
        Linear sweep disassembly of a section.

        Decodes all bytes sequentially. On invalid byte sequences,
        skips forward and resumes decoding.

        Returns number of instructions decoded.
        """
        data = self.image.get_section_data(section)
        if not data:
            return 0

        va_start = section.virtual_addr
        total = len(data)
        count = 0
        offset = 0

        while offset < total:
            # Decode from current offset to end of section
            chunk = data[offset:]
            chunk_va = va_start + offset
            last_end_offset = offset

            for cs_insn in self._cs.disasm(chunk, chunk_va):
                insn = self._classify_instruction(cs_insn)
                self.instructions[insn.address] = insn
                count += 1
                last_end_offset = (cs_insn.address + cs_insn.size) - va_start

            # Progress reporting
            if progress_callback and last_end_offset > offset:
                progress_callback(min(last_end_offset, total), total)

            if last_end_offset > offset:
                offset = last_end_offset
            else:
                # Capstone couldn't decode anything - skip one byte
                offset += 1

            # If we've decoded to near the end, we're done
            if offset >= total:
                break

        # Invalidate sorted address cache
        self._sorted_addrs = None

        if progress_callback:
            progress_callback(total, total)

        return count

    def recursive_descent(self, start_addresses: List[int],
                          section_bounds: List[Tuple[int, int]]) -> Set[int]:
        """
        Recursive descent from given start addresses.

        Follows control flow to determine reachable instructions.

        Returns set of reachable instruction addresses.
        """
        reachable: Set[int] = set()
        worklist = list(start_addresses)
        visited_starts: Set[int] = set()

        def in_bounds(addr: int) -> bool:
            return any(s <= addr < e for s, e in section_bounds)

        while worklist:
            addr = worklist.pop()
            if addr in visited_starts:
                continue
            visited_starts.add(addr)

            # Walk the instruction stream linearly from this start point
            while True:
                if addr in reachable:
                    break  # Already explored from here
                if not in_bounds(addr):
                    break

                insn = self.instructions.get(addr)
                if insn is None:
                    break

                reachable.add(addr)

                # Follow call targets (but continue past the call)
                if insn.is_call and insn.call_target is not None:
                    if in_bounds(insn.call_target) and insn.call_target not in visited_starts:
                        worklist.append(insn.call_target)

                # Follow conditional jump targets (and continue fall-through)
                if insn.is_cond_jump and insn.jump_target is not None:
                    if in_bounds(insn.jump_target) and insn.jump_target not in visited_starts:
                        worklist.append(insn.jump_target)

                # Unconditional jump: follow target, stop linear walk
                if insn.is_jump:
                    if insn.jump_target is not None and in_bounds(insn.jump_target):
                        if insn.jump_target not in visited_starts:
                            worklist.append(insn.jump_target)
                    break

                if insn.is_ret:
                    break

                addr = insn.end_address

        return reachable

    def get_instruction(self, address: int) -> Optional[Instruction]:
        """Get a decoded instruction by address."""
        return self.instructions.get(address)

    def _ensure_sorted_addrs(self):
        """Build sorted address list if not cached."""
        if self._sorted_addrs is None:
            self._sorted_addrs = sorted(self.instructions.keys())

    def get_instructions_in_range(self, start: int, end: int) -> List[Instruction]:
        """Get all instructions in address range [start, end), sorted."""
        self._ensure_sorted_addrs()
        import bisect
        lo = bisect.bisect_left(self._sorted_addrs, start)
        hi = bisect.bisect_left(self._sorted_addrs, end)
        return [self.instructions[self._sorted_addrs[i]]
                for i in range(lo, hi)]

    def instruction_count(self) -> int:
        return len(self.instructions)

    def get_call_targets(self) -> Set[int]:
        """Return all direct call target addresses."""
        targets = set()
        for insn in self.instructions.values():
            if insn.call_target is not None:
                targets.add(insn.call_target)
        return targets

    def get_indirect_call_refs(self) -> Dict[int, List[int]]:
        """
        Return memory addresses referenced by indirect calls.
        Maps: memory_address -> [calling_instruction_addresses]
        """
        refs: Dict[int, List[int]] = {}
        for insn in self.instructions.values():
            if insn.is_call and insn.memory_ref is not None:
                refs.setdefault(insn.memory_ref, []).append(insn.address)
        return refs
