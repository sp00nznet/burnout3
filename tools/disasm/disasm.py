"""
Main disassembly orchestrator.

Coordinates all analysis passes and produces the final output.
"""

import time
from pathlib import Path
from typing import List, Optional

from . import config
from .loader import load_image, BinaryImage, SectionInfo
from .engine import DisasmEngine
from .functions import FunctionDetector
from .xrefs import build_xrefs, XRefTracker
from .labels import (
    LabelManager, populate_kernel_labels, populate_entry_point,
    extract_strings, populate_string_labels,
)
from .output import OutputWriter, print_stats
from .cache import AnalysisCache


class Disassembler:
    """
    Top-level disassembly orchestrator.

    Usage:
        d = Disassembler("path/to/default.xbe")
        d.run()
    """

    def __init__(self, xbe_path: str,
                 analysis_json: Optional[str] = None,
                 output_dir: Optional[str] = None,
                 text_only: bool = False,
                 stats_only: bool = False,
                 verbose: bool = False,
                 force: bool = False):
        self.xbe_path = xbe_path
        self.analysis_json = analysis_json
        self.output_dir = output_dir or config.DEFAULT_OUTPUT_DIR
        self.text_only = text_only
        self.stats_only = stats_only
        self.verbose = verbose
        self.force = force

        # Components (initialized during run)
        self.image: Optional[BinaryImage] = None
        self.engine: Optional[DisasmEngine] = None
        self.labels: Optional[LabelManager] = None
        self.xrefs: Optional[XRefTracker] = None
        self.func_detector: Optional[FunctionDetector] = None
        self.strings: List[dict] = []

    def run(self) -> bool:
        """
        Execute the full disassembly pipeline.

        Returns True on success.
        """
        t_start = time.time()

        # Check cache
        cache = AnalysisCache(self.output_dir)
        if not self.force:
            json_path = self._find_analysis_json()
            if json_path and cache.is_valid(self.xbe_path, json_path,
                                             self.text_only):
                last_time = cache.get_last_run_time()
                print(f"Cache hit - results unchanged (last run: "
                      f"{last_time:.1f}s)")
                if self.stats_only:
                    self._load_and_print_cached_stats()
                return True

        # Phase 1: Load
        if self.verbose:
            print("Phase 1: Loading binary image...")
        self.image = load_image(self.xbe_path, self.analysis_json)
        if self.verbose:
            print(f"  Loaded: {self.image.filepath}")
            print(f"  Base: 0x{self.image.base_address:08X}  "
                  f"Entry: 0x{self.image.entry_point:08X}")
            print(f"  Sections: {len(self.image.sections)}  "
                  f"Kernel imports: {len(self.image.kernel_imports)}")

        # Determine sections to analyze
        sections = self._get_target_sections()
        if self.verbose:
            print(f"  Target sections: {', '.join(s.name for s in sections)}")

        # Phase 2: Labels (pre-populate known symbols)
        if self.verbose:
            print("\nPhase 2: Populating labels...")
        self.labels = LabelManager()
        populate_entry_point(self.labels, self.image)
        ki_count = populate_kernel_labels(self.labels, self.image)
        if self.verbose:
            print(f"  Kernel import labels: {ki_count}")

        # Extract strings from .rdata
        self.strings = extract_strings(self.image)
        str_count = populate_string_labels(self.labels, self.strings)
        if self.verbose:
            print(f"  String labels: {str_count}")
            print(f"  Total labels: {self.labels.count()}")

        # Phase 3: Disassembly (linear sweep)
        if self.verbose:
            print("\nPhase 3: Linear sweep disassembly...")
        self.engine = DisasmEngine(self.image)

        total_insns = 0
        for sec in sections:
            if self.verbose:
                print(f"  {sec.name}: 0x{sec.virtual_addr:08X} "
                      f"({sec.virtual_size / 1024:.1f} KB)...", end="", flush=True)

            def progress(done, total):
                if self.verbose:
                    pct = done * 100 // total
                    print(f"\r  {sec.name}: 0x{sec.virtual_addr:08X} "
                          f"({sec.virtual_size / 1024:.1f} KB)... "
                          f"{pct}%", end="", flush=True)

            n = self.engine.linear_sweep(sec, progress_callback=progress)
            total_insns += n
            if self.verbose:
                print(f"\r  {sec.name}: {n:,d} instructions")

        if self.verbose:
            print(f"  Total: {total_insns:,d} instructions")

        # Phase 4: Cross-references
        if self.verbose:
            print("\nPhase 4: Building cross-references...")
        self.xrefs = build_xrefs(self.engine, self.image)
        if self.verbose:
            counts = self.xrefs.count_by_type()
            print(f"  Total xrefs: {self.xrefs.count():,d}")
            for xtype, count in sorted(counts.items()):
                print(f"    {xtype}: {count:,d}")

        # Phase 5: Function detection
        if self.verbose:
            print("\nPhase 5: Detecting functions...")
        self.func_detector = FunctionDetector(
            self.engine, self.image, self.xrefs, self.labels)
        num_funcs = self.func_detector.detect_all(sections)
        if self.verbose:
            summary = self.func_detector.summary()
            print(f"  Total functions: {num_funcs:,d}")
            for method, count in sorted(
                    summary["by_detection_method"].items()):
                print(f"    {method}: {count:,d}")

        # Phase 6: Recursive descent validation
        if self.verbose:
            print("\nPhase 6: Recursive descent validation...")
        start_addrs = [self.image.entry_point]
        start_addrs.extend(self.func_detector.functions.keys())
        section_bounds = [
            (s.virtual_addr, s.virtual_addr + s.virtual_size)
            for s in sections
        ]
        reachable = self.engine.recursive_descent(start_addrs, section_bounds)
        if self.verbose:
            coverage = len(reachable) / total_insns * 100 if total_insns else 0
            print(f"  Reachable instructions: {len(reachable):,d} "
                  f"({coverage:.1f}%)")

        elapsed = time.time() - t_start

        # Print stats
        if self.stats_only or self.verbose:
            print_stats(self.engine, self.func_detector, self.xrefs,
                        self.labels, self.strings, self.image)
            print(f"\n  Elapsed: {elapsed:.2f}s")

        # Phase 7: Output
        if not self.stats_only:
            if self.verbose:
                print(f"\nPhase 7: Writing output to {self.output_dir}/...")
            writer = OutputWriter(
                self.output_dir, self.engine, self.func_detector,
                self.xrefs, self.labels, self.image, self.strings)
            writer.write_all(sections_to_disasm=sections, verbose=self.verbose)

            # Save cache
            json_path = self._find_analysis_json()
            if json_path:
                cache.save(self.xbe_path, json_path, self.text_only, elapsed)

            if self.verbose:
                print(f"\n  Output written to {self.output_dir}/")

        print(f"Done in {elapsed:.2f}s")
        return True

    def _get_target_sections(self) -> List[SectionInfo]:
        """Determine which sections to disassemble."""
        if self.text_only:
            text = self.image.get_section(".text")
            if text is None:
                raise ValueError("No .text section found")
            return [text]

        # All executable sections with code
        return self.image.get_code_sections()

    def _find_analysis_json(self) -> Optional[str]:
        """Find the analysis JSON file path."""
        if self.analysis_json:
            return self.analysis_json

        candidates = [
            Path(self.xbe_path).parent / "burnout3_analysis.json",
            Path("tools/xbe_parser/burnout3_analysis.json"),
        ]
        for p in candidates:
            if p.exists():
                return str(p)
        return None

    def _load_and_print_cached_stats(self) -> None:
        """Load and print stats from cached summary.json."""
        summary_path = Path(self.output_dir) / "summary.json"
        if not summary_path.exists():
            print("  (no cached summary available)")
            return

        import json
        with open(summary_path) as f:
            summary = json.load(f)

        print(f"\n{'=' * 60}")
        print(f"  Cached Disassembly Summary")
        print(f"{'=' * 60}")
        print(f"  Binary: {summary.get('binary', 'N/A')}")
        print(f"  Instructions: {summary.get('total_instructions', 0):,d}")
        print(f"  Functions: {summary.get('total_functions', 0):,d}")
        print(f"  Cross-references: {summary.get('total_xrefs', 0):,d}")
        print(f"  Labels: {summary.get('total_labels', 0):,d}")
        print(f"  Strings: {summary.get('total_strings', 0):,d}")
        print(f"{'=' * 60}")
