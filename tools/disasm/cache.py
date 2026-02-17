"""
Incremental analysis cache for the disassembler.

Uses SHA-256 hashing to detect when the input binary has changed,
allowing fast re-runs when nothing has changed.
"""

import hashlib
import json
import os
import time
from pathlib import Path
from typing import Dict, Optional

from . import config


class AnalysisCache:
    """
    SHA-256 based cache for disassembly results.

    Stores a hash of the input binary and the analysis JSON path.
    On subsequent runs, if the hash matches, the cached results
    can be loaded instead of re-analyzing.
    """

    def __init__(self, output_dir: str):
        self.output_dir = Path(output_dir)
        self.cache_file = self.output_dir / config.CACHE_FILENAME
        self._cache_data: Optional[dict] = None

    def _load_cache(self) -> Optional[dict]:
        """Load existing cache data."""
        if self._cache_data is not None:
            return self._cache_data

        if not self.cache_file.exists():
            return None

        try:
            with open(self.cache_file) as f:
                data = json.load(f)
            if data.get("version") != config.CACHE_VERSION:
                return None
            self._cache_data = data
            return data
        except (json.JSONDecodeError, KeyError):
            return None

    def _hash_file(self, filepath: str) -> str:
        """Compute SHA-256 hash of a file."""
        sha256 = hashlib.sha256()
        with open(filepath, 'rb') as f:
            while True:
                chunk = f.read(65536)
                if not chunk:
                    break
                sha256.update(chunk)
        return sha256.hexdigest()

    def is_valid(self, xbe_path: str, analysis_json_path: str,
                 text_only: bool = False) -> bool:
        """
        Check if cached results are still valid.

        Returns True if the cache exists and the input files haven't changed.
        """
        cache = self._load_cache()
        if cache is None:
            return False

        # Check XBE hash
        current_hash = self._hash_file(xbe_path)
        if cache.get("xbe_hash") != current_hash:
            return False

        # Check analysis JSON hash
        json_hash = self._hash_file(analysis_json_path)
        if cache.get("json_hash") != json_hash:
            return False

        # Check text_only flag matches
        if cache.get("text_only") != text_only:
            return False

        # Verify output files exist
        required_files = [
            "summary.json", "functions.json", "xrefs.json",
            "strings.json", "labels.json",
        ]
        for fname in required_files:
            if not (self.output_dir / fname).exists():
                return False

        return True

    def save(self, xbe_path: str, analysis_json_path: str,
             text_only: bool, elapsed_seconds: float) -> None:
        """
        Save cache metadata after a successful analysis run.
        """
        self.output_dir.mkdir(parents=True, exist_ok=True)

        cache_data = {
            "version": config.CACHE_VERSION,
            "xbe_hash": self._hash_file(xbe_path),
            "json_hash": self._hash_file(analysis_json_path),
            "text_only": text_only,
            "timestamp": time.time(),
            "elapsed_seconds": elapsed_seconds,
        }

        with open(self.cache_file, 'w') as f:
            json.dump(cache_data, f, indent=2)

        self._cache_data = cache_data

    def get_last_run_time(self) -> Optional[float]:
        """Get the elapsed time from the last cached run."""
        cache = self._load_cache()
        if cache:
            return cache.get("elapsed_seconds")
        return None

    def invalidate(self) -> None:
        """Delete the cache file."""
        if self.cache_file.exists():
            os.remove(self.cache_file)
        self._cache_data = None
