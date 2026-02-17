"""
Symbol and label management for the disassembler.

Manages all named locations: kernel imports, string references,
auto-named functions (sub_XXXXXXXX), and user-defined labels.
"""

from dataclasses import dataclass, field
from enum import Enum
from typing import Dict, List, Optional, Set

from .loader import BinaryImage, KernelImport


class LabelType(Enum):
    KERNEL_IMPORT = "kernel_import"
    FUNCTION = "function"
    STRING_REF = "string_ref"
    DATA = "data"
    ENTRY_POINT = "entry_point"
    THUNK = "thunk"


@dataclass
class Label:
    """A named location in the binary."""
    address: int
    name: str
    label_type: LabelType
    section: str = ""
    confidence: float = 1.0

    def to_dict(self) -> dict:
        return {
            "address": f"0x{self.address:08X}",
            "name": self.name,
            "type": self.label_type.value,
            "section": self.section,
            "confidence": self.confidence,
        }


class LabelManager:
    """
    Central label/symbol table for the disassembly.

    Thread-safe for read access after initial population.
    """

    def __init__(self):
        self._labels: Dict[int, Label] = {}
        self._names: Dict[str, int] = {}  # name -> address reverse lookup
        self._func_counter = 0

    def add(self, label: Label) -> None:
        """Add or update a label. Higher confidence wins on conflict."""
        existing = self._labels.get(label.address)
        if existing is not None:
            # Keep higher confidence, prefer named over auto-generated
            if (label.confidence > existing.confidence or
                    (label.confidence == existing.confidence and
                     not label.name.startswith("sub_") and
                     existing.name.startswith("sub_"))):
                # Remove old name mapping
                if existing.name in self._names:
                    del self._names[existing.name]
            else:
                return  # Keep existing

        self._labels[label.address] = label
        self._names[label.name] = label.address

    def get(self, address: int) -> Optional[Label]:
        """Get label at address."""
        return self._labels.get(address)

    def get_by_name(self, name: str) -> Optional[Label]:
        """Look up label by name."""
        addr = self._names.get(name)
        if addr is not None:
            return self._labels.get(addr)
        return None

    def has(self, address: int) -> bool:
        """Check if an address has a label."""
        return address in self._labels

    def get_name(self, address: int) -> Optional[str]:
        """Get the name for an address, or None."""
        label = self._labels.get(address)
        return label.name if label else None

    def get_display_name(self, address: int) -> str:
        """Get display name: label name if exists, else hex address."""
        label = self._labels.get(address)
        if label:
            return label.name
        return f"0x{address:08X}"

    def auto_name_function(self, address: int, section: str = "",
                           confidence: float = 0.5) -> Label:
        """Create an auto-named function label (sub_XXXXXXXX)."""
        existing = self._labels.get(address)
        if existing and existing.label_type == LabelType.FUNCTION:
            return existing

        name = f"sub_{address:08X}"
        label = Label(
            address=address,
            name=name,
            label_type=LabelType.FUNCTION,
            section=section,
            confidence=confidence,
        )
        self.add(label)
        return label

    def all_labels(self) -> List[Label]:
        """Return all labels sorted by address."""
        return sorted(self._labels.values(), key=lambda l: l.address)

    def labels_in_range(self, start: int, end: int) -> List[Label]:
        """Return labels within an address range [start, end)."""
        return [l for l in self._labels.values()
                if start <= l.address < end]

    def count(self) -> int:
        return len(self._labels)

    def count_by_type(self, label_type: LabelType) -> int:
        return sum(1 for l in self._labels.values()
                   if l.label_type == label_type)

    def to_list(self) -> List[dict]:
        """Export all labels as a list of dicts."""
        return [l.to_dict() for l in self.all_labels()]


def populate_kernel_labels(labels: LabelManager, image: BinaryImage) -> int:
    """
    Add labels for all kernel import thunks.

    Returns the number of kernel import labels added.
    """
    count = 0
    for ki in image.kernel_imports:
        label = Label(
            address=ki.thunk_addr,
            name=f"xbox_{ki.name}",
            label_type=LabelType.KERNEL_IMPORT,
            section=".rdata",
            confidence=1.0,
        )
        labels.add(label)
        count += 1
    return count


def populate_entry_point(labels: LabelManager, image: BinaryImage) -> None:
    """Add label for the XBE entry point."""
    labels.add(Label(
        address=image.entry_point,
        name="xbe_entry_point",
        label_type=LabelType.ENTRY_POINT,
        section=".text",
        confidence=1.0,
    ))


def extract_strings(image: BinaryImage, min_length: int = 4,
                    max_length: int = 256) -> List[dict]:
    """
    Extract printable ASCII strings from .rdata section.

    Returns list of {"address": int, "string": str, "length": int}.
    """
    rdata = image.get_section(".rdata")
    if rdata is None:
        return []

    data = image.get_section_data(rdata)
    strings = []
    i = 0

    while i < len(data):
        # Look for runs of printable ASCII
        start = i
        while i < len(data) and i - start < max_length:
            b = data[i]
            if 0x20 <= b < 0x7F or b in (0x09, 0x0A, 0x0D):  # printable + whitespace
                i += 1
            else:
                break

        length = i - start
        if length >= min_length and i < len(data) and data[i] == 0x00:
            s = data[start:i].decode('ascii', errors='replace')
            strings.append({
                "address": rdata.virtual_addr + start,
                "string": s,
                "length": length,
            })
            i += 1  # skip null terminator
        else:
            i = start + 1 if i == start else i + 1

    return strings


def populate_string_labels(labels: LabelManager,
                           string_refs: List[dict]) -> int:
    """Add labels for string references."""
    count = 0
    for sr in string_refs:
        # Create a sanitized name from string content
        s = sr["string"][:32].replace(" ", "_").replace("/", "_")
        s = ''.join(c for c in s if c.isalnum() or c == '_')
        if not s:
            s = f"str_{sr['address']:08X}"
        else:
            s = f"str_{s}"

        labels.add(Label(
            address=sr["address"],
            name=s,
            label_type=LabelType.STRING_REF,
            section=".rdata",
            confidence=0.8,
        ))
        count += 1
    return count
