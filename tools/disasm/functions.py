"""
Function boundary detection for the disassembler.

Implements multi-pass function detection with confidence scoring:
1. Known addresses (entry point)
2. Standard prologues (push ebp; mov ebp, esp)
3. CC padding boundaries (CC run after ret)
4. Call targets (destinations of call instructions)
5. Cross-validation and overlap resolution
"""

from dataclasses import dataclass, field
from typing import Dict, List, Optional, Set, Tuple

from . import config
from .engine import DisasmEngine, Instruction
from .loader import BinaryImage, SectionInfo
from .xrefs import XRefTracker
from .labels import LabelManager, Label, LabelType


@dataclass
class Function:
    """A detected function with boundaries and metadata."""
    start: int
    end: int           # Address after last instruction
    name: str
    section: str = ""
    confidence: float = 0.0
    detection_method: str = ""

    # Call graph data
    calls_to: List[int] = field(default_factory=list)     # Functions this calls
    called_by: List[int] = field(default_factory=list)     # Functions that call this

    # Instruction stats
    num_instructions: int = 0
    has_prologue: bool = False

    @property
    def size(self) -> int:
        return self.end - self.start

    def to_dict(self) -> dict:
        return {
            "start": f"0x{self.start:08X}",
            "end": f"0x{self.end:08X}",
            "size": self.size,
            "name": self.name,
            "section": self.section,
            "confidence": self.confidence,
            "detection_method": self.detection_method,
            "num_instructions": self.num_instructions,
            "has_prologue": self.has_prologue,
            "calls_to": [f"0x{a:08X}" for a in self.calls_to],
            "called_by": [f"0x{a:08X}" for a in self.called_by],
        }


class FunctionDetector:
    """
    Multi-pass function boundary detector.

    Identifies function start addresses through multiple heuristics,
    then determines function boundaries by following instruction flow
    until the next function or a terminal instruction.
    """

    def __init__(self, engine: DisasmEngine, image: BinaryImage,
                 xrefs: XRefTracker, labels: LabelManager):
        self.engine = engine
        self.image = image
        self.xrefs = xrefs
        self.labels = labels

        # Candidate function starts: address -> (confidence, method)
        self._candidates: Dict[int, Tuple[float, str]] = {}

        # Final function list
        self.functions: Dict[int, Function] = {}

    def detect_all(self, sections: Optional[List[SectionInfo]] = None) -> int:
        """
        Run all detection passes and build the function database.

        Args:
            sections: Sections to analyze. If None, uses all executable sections.

        Returns:
            Number of functions detected.
        """
        if sections is None:
            sections = self.image.get_code_sections()

        # Pass 1: Known addresses
        self._pass_known_addresses()

        # Pass 2: Prologue patterns
        for sec in sections:
            self._pass_prologues(sec)

        # Pass 3: CC padding boundaries
        for sec in sections:
            self._pass_cc_boundaries(sec)

        # Pass 4: Call targets
        self._pass_call_targets(sections)

        # Pass 5: Build functions from candidates
        self._build_functions(sections)

        # Populate call graph
        self._build_call_graph()

        return len(self.functions)

    def _add_candidate(self, addr: int, confidence: float, method: str) -> None:
        """Add a function start candidate, keeping highest confidence."""
        existing = self._candidates.get(addr)
        if existing is None or confidence > existing[0]:
            self._candidates[addr] = (confidence, method)

    def _pass_known_addresses(self) -> None:
        """Pass 1: Add known function addresses."""
        # Entry point
        self._add_candidate(
            self.image.entry_point,
            config.CONFIDENCE_KNOWN,
            "entry_point"
        )

    def _pass_prologues(self, section: SectionInfo) -> None:
        """
        Pass 2: Scan for standard function prologues.

        Looks for: push ebp (0x55); mov ebp, esp (0x8BEC or 0x89E5)
        """
        data = self.image.get_section_data(section)
        if not data:
            return

        va_start = section.virtual_addr
        i = 0
        while i < len(data) - 2:
            # Check for push ebp; mov ebp, esp
            if data[i] == 0x55:
                if (i + 2 < len(data) and
                        data[i + 1] == 0x8B and data[i + 2] == 0xEC):
                    addr = va_start + i
                    # Verify this address has a decoded instruction
                    if addr in self.engine.instructions:
                        self._add_candidate(
                            addr,
                            config.CONFIDENCE_PROLOGUE,
                            "prologue"
                        )
                    i += 3
                    continue
                elif (i + 2 < len(data) and
                      data[i + 1] == 0x89 and data[i + 2] == 0xE5):
                    addr = va_start + i
                    if addr in self.engine.instructions:
                        self._add_candidate(
                            addr,
                            config.CONFIDENCE_PROLOGUE,
                            "prologue_alt"
                        )
                    i += 3
                    continue
            i += 1

    def _pass_cc_boundaries(self, section: SectionInfo) -> None:
        """
        Pass 3: Find function boundaries at CC padding.

        Pattern: ret instruction, followed by one or more 0xCC bytes,
        followed by the start of the next function.
        """
        data = self.image.get_section_data(section)
        if not data:
            return

        va_start = section.virtual_addr
        i = 0

        while i < len(data):
            # Look for CC padding runs
            if data[i] == config.CC_PADDING:
                cc_start = i
                while i < len(data) and data[i] == config.CC_PADDING:
                    i += 1

                cc_run_length = i - cc_start

                if cc_run_length >= config.MIN_CC_RUN and i < len(data):
                    # Check if instruction before CC run was a ret
                    before_addr = va_start + cc_start
                    # Look for a ret instruction ending right at the CC run
                    found_ret = False
                    for check_offset in range(1, 4):  # ret can be 1-3 bytes
                        check_addr = before_addr - check_offset
                        insn = self.engine.get_instruction(check_addr)
                        if insn and insn.is_ret and insn.end_address == before_addr:
                            found_ret = True
                            break

                    if found_ret:
                        next_addr = va_start + i
                        if next_addr in self.engine.instructions:
                            self._add_candidate(
                                next_addr,
                                config.CONFIDENCE_CC_BOUNDARY,
                                "cc_boundary"
                            )
            else:
                i += 1

    def _pass_call_targets(self, sections: List[SectionInfo]) -> None:
        """
        Pass 4: Add destinations of direct call instructions as function starts.
        """
        # Build set of valid code ranges
        code_ranges = set()
        for sec in sections:
            for addr in range(sec.virtual_addr,
                              sec.virtual_addr + sec.virtual_size):
                code_ranges.add(addr)

        call_targets = self.engine.get_call_targets()
        for target in call_targets:
            # Verify target is in a code section and has a decoded instruction
            if target in self.engine.instructions:
                section = self.image.get_section_at_va(target)
                if section and section.executable:
                    self._add_candidate(
                        target,
                        config.CONFIDENCE_CALL_TARGET,
                        "call_target"
                    )

    def _build_functions(self, sections: List[SectionInfo]) -> None:
        """
        Pass 5: Build Function objects from candidates.

        Determines function boundaries by finding the extent of each
        function (up to the next function start or unreachable point).
        """
        # Sort candidates by address
        sorted_starts = sorted(self._candidates.keys())
        if not sorted_starts:
            return

        # Build section boundary lookup
        sec_ranges = {}
        for sec in sections:
            sec_ranges[sec.name] = (sec.virtual_addr,
                                    sec.virtual_addr + sec.virtual_size)

        # Create functions
        for idx, start_addr in enumerate(sorted_starts):
            confidence, method = self._candidates[start_addr]

            # Determine section
            section = self.image.get_section_at_va(start_addr)
            sec_name = section.name if section else ""

            # Determine end address:
            # Walk instructions until we hit the next function start,
            # leave the section, or reach an unconditional terminator
            # with no fall-through.
            if idx + 1 < len(sorted_starts):
                next_func = sorted_starts[idx + 1]
            else:
                next_func = None

            # Section end boundary
            sec_end = None
            if section:
                sec_end = section.virtual_addr + section.virtual_size

            end_addr = self._find_function_end(start_addr, next_func, sec_end)

            # Count instructions
            insns = self.engine.get_instructions_in_range(start_addr, end_addr)
            num_insns = len(insns)

            if num_insns == 0:
                continue

            # Check for prologue
            first_insn = self.engine.get_instruction(start_addr)
            has_prologue = (first_insn is not None and
                            first_insn.mnemonic == "push" and
                            first_insn.op_str == "ebp")

            # Get or create name
            label = self.labels.get(start_addr)
            if label:
                name = label.name
            else:
                name = f"sub_{start_addr:08X}"
                self.labels.auto_name_function(
                    start_addr, sec_name, confidence)

            func = Function(
                start=start_addr,
                end=end_addr,
                name=name,
                section=sec_name,
                confidence=confidence,
                detection_method=method,
                num_instructions=num_insns,
                has_prologue=has_prologue,
            )
            self.functions[start_addr] = func

    def _find_function_end(self, start: int, next_func: Optional[int],
                           sec_end: Optional[int]) -> int:
        """
        Determine where a function ends.

        Walks forward from start, tracking the furthest reachable point
        through fall-through and internal jumps.
        """
        max_addr = start
        addr = start

        # Upper bound
        upper = sec_end if sec_end else start + 0x100000
        if next_func and next_func < upper:
            upper = next_func

        while addr < upper:
            insn = self.engine.get_instruction(addr)
            if insn is None:
                break

            end = insn.end_address
            if end > max_addr:
                max_addr = end

            # Track internal forward jumps to extend function bounds
            if insn.is_cond_jump and insn.jump_target is not None:
                target = insn.jump_target
                if start <= target < upper and target > max_addr:
                    # This jump goes forward within bounds, extend
                    max_addr = target

            if insn.is_ret or (insn.is_jump and not insn.is_cond_jump):
                # Check if we've covered all internal jump targets
                if addr + insn.size >= max_addr:
                    break
                # There might be more code after (jumped over)
                addr = insn.end_address
                continue

            addr = insn.end_address

        return max_addr

    def _build_call_graph(self) -> None:
        """Populate calls_to and called_by for all functions."""
        func_starts = set(self.functions.keys())

        for func in self.functions.values():
            insns = self.engine.get_instructions_in_range(func.start, func.end)
            callees = set()
            for insn in insns:
                if insn.call_target is not None:
                    callees.add(insn.call_target)

            func.calls_to = sorted(callees)

            for callee_addr in callees:
                callee = self.functions.get(callee_addr)
                if callee is not None:
                    callee.called_by.append(func.start)

        # Sort called_by lists
        for func in self.functions.values():
            func.called_by = sorted(set(func.called_by))

    def get_function_at(self, addr: int) -> Optional[Function]:
        """Get the function containing an address."""
        # First check direct match
        if addr in self.functions:
            return self.functions[addr]
        # Search for containing function
        for func in self.functions.values():
            if func.start <= addr < func.end:
                return func
        return None

    def get_functions_in_section(self, section_name: str) -> List[Function]:
        """Get all functions in a section, sorted by address."""
        return sorted(
            [f for f in self.functions.values() if f.section == section_name],
            key=lambda f: f.start
        )

    def summary(self) -> dict:
        """Return summary statistics."""
        by_method: Dict[str, int] = {}
        by_section: Dict[str, int] = {}
        total_insns = 0
        with_prologue = 0

        for func in self.functions.values():
            by_method[func.detection_method] = by_method.get(
                func.detection_method, 0) + 1
            by_section[func.section] = by_section.get(func.section, 0) + 1
            total_insns += func.num_instructions
            if func.has_prologue:
                with_prologue += 1

        return {
            "total_functions": len(self.functions),
            "total_instructions_in_functions": total_insns,
            "with_prologue": with_prologue,
            "by_detection_method": by_method,
            "by_section": by_section,
        }
