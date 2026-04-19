#!/usr/bin/env python3
"""Prepare frontend assets for LittleFS upload.

Behavior:
- Targets HTML/CSS/JS files under data/.
- Applies conservative minification (safe whitespace/comment cleanup).
- Produces deterministic precompressed .gz companions.
- Keeps original source files untouched for maintainability.
"""

from __future__ import annotations

import argparse
import gzip
import hashlib
import json
import re
from dataclasses import dataclass
from pathlib import Path

TARGET_EXTENSIONS = {".html", ".css", ".js"}
CACHE_SCHEMA_VERSION = 1
CACHE_SIGNATURE = "securelock-minify-v2-gzip9-mtime0"
DEFAULT_CACHE_DIRNAME = ".cache"
DEFAULT_CACHE_FILENAME = "frontend_assets_cache.json"


@dataclass
class Stats:
    processed_files: int = 0
    updated_gzip_files: int = 0
    skipped_unchanged_files: int = 0
    removed_stale_gzip_files: int = 0
    original_bytes: int = 0
    payload_bytes: int = 0
    gzip_bytes: int = 0


def resolve_default_cache_file(data_dir: Path) -> Path:
    return data_dir.parent / DEFAULT_CACHE_DIRNAME / DEFAULT_CACHE_FILENAME


def file_sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def load_cache(cache_file: Path) -> dict:
    if not cache_file.exists() or not cache_file.is_file():
        return {
            "schemaVersion": CACHE_SCHEMA_VERSION,
            "signature": CACHE_SIGNATURE,
            "files": {},
        }

    try:
        content = json.loads(cache_file.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return {
            "schemaVersion": CACHE_SCHEMA_VERSION,
            "signature": CACHE_SIGNATURE,
            "files": {},
        }

    if (
        int(content.get("schemaVersion", 0)) != CACHE_SCHEMA_VERSION
        or str(content.get("signature", "")) != CACHE_SIGNATURE
        or not isinstance(content.get("files"), dict)
    ):
        return {
            "schemaVersion": CACHE_SCHEMA_VERSION,
            "signature": CACHE_SIGNATURE,
            "files": {},
        }

    return content


def save_cache(cache_file: Path, cache_data: dict) -> None:
    cache_file.parent.mkdir(parents=True, exist_ok=True)
    temp_file = cache_file.with_suffix(cache_file.suffix + ".tmp")
    temp_file.write_text(
        json.dumps(cache_data, indent=2, sort_keys=True),
        encoding="utf-8",
    )
    temp_file.replace(cache_file)


def minify_html(content: str) -> str:
    def _comment_replacer(match: re.Match[str]) -> str:
        body = (match.group(1) or "").strip()
        # Preserve IE conditional comments (rare but harmless to keep).
        if body.startswith("[if") or body.startswith("![endif"):
            return match.group(0)
        return ""

    compact = re.sub(r"<!--(.*?)-->", _comment_replacer, content, flags=re.DOTALL)
    compact = re.sub(r">\s+<", "><", compact)
    compact = re.sub(r"[ \t]+\n", "\n", compact)
    compact = re.sub(r"\n{2,}", "\n", compact)
    compact = compact.strip()
    return compact + "\n" if compact else ""


def minify_css(content: str) -> str:
    compact = re.sub(r"/\*.*?\*/", "", content, flags=re.DOTALL)
    compact = re.sub(r"\s+", " ", compact)
    compact = re.sub(r"\s*([{}:;,>+~])\s*", r"\1", compact)
    compact = compact.replace(";}", "}")
    compact = compact.strip()
    return compact + "\n" if compact else ""


def minify_js(content: str) -> str:
    # Conservative JS minification to avoid semantic changes:
    # - trim trailing whitespace
    # - collapse excessive blank lines
    lines = [line.rstrip() for line in content.splitlines()]
    collapsed: list[str] = []
    previous_blank = False

    for line in lines:
        is_blank = len(line.strip()) == 0
        if is_blank and previous_blank:
            continue
        collapsed.append(line)
        previous_blank = is_blank

    compact = "\n".join(collapsed).strip()
    return compact + "\n" if compact else ""


def build_payload(path: Path, content: str) -> str:
    suffix = path.suffix.lower()
    if suffix == ".html":
        return minify_html(content)
    if suffix == ".css":
        return minify_css(content)
    if suffix == ".js":
        return minify_js(content)
    return content


def should_process(path: Path) -> bool:
    if not path.is_file():
        return False
    if path.suffix.lower() not in TARGET_EXTENSIONS:
        return False
    # Skip already generated compressed files.
    if path.name.endswith(".gz"):
        return False
    return True


def write_if_changed(path: Path, payload: bytes) -> bool:
    if path.exists() and path.read_bytes() == payload:
        return False
    path.write_bytes(payload)
    return True


def remove_stale_gzip_files(data_dir: Path) -> int:
    removed = 0
    for gz_file in data_dir.rglob("*.gz"):
        base_candidate = Path(str(gz_file)[:-3])
        if base_candidate.suffix.lower() not in TARGET_EXTENSIONS:
            continue
        if base_candidate.exists():
            continue
        gz_file.unlink(missing_ok=True)
        removed += 1
    return removed


def optimize_assets(data_dir: Path, cache_file: Path) -> Stats:
    stats = Stats()
    cache = load_cache(cache_file)
    cached_files = cache.get("files", {})
    next_cached_files: dict[str, dict[str, int | str]] = {}
    managed_files = sorted(path for path in data_dir.rglob("*") if should_process(path))

    for source_path in managed_files:
        cache_key = source_path.relative_to(data_dir).as_posix()
        original_bytes = source_path.read_bytes()
        source_hash = file_sha256(original_bytes)
        gz_path = source_path.with_suffix(source_path.suffix + ".gz")

        cached_entry = cached_files.get(cache_key)
        if (
            isinstance(cached_entry, dict)
            and str(cached_entry.get("sourceHash", "")) == source_hash
            and gz_path.exists()
        ):
            payload_size = int(cached_entry.get("payloadSize", len(original_bytes)))
            gzip_size = int(cached_entry.get("gzipSize", gz_path.stat().st_size))

            stats.processed_files += 1
            stats.skipped_unchanged_files += 1
            stats.original_bytes += len(original_bytes)
            stats.payload_bytes += payload_size
            stats.gzip_bytes += gzip_size

            next_cached_files[cache_key] = {
                "sourceHash": source_hash,
                "payloadSize": payload_size,
                "gzipSize": gzip_size,
            }
            continue

        try:
            original_text = original_bytes.decode("utf-8")
        except UnicodeDecodeError:
            # Non-UTF8 files are skipped to avoid corruption.
            continue

        candidate_payload_text = build_payload(source_path, original_text)
        candidate_payload_bytes = candidate_payload_text.encode("utf-8")

        # Only use minified payload when it is genuinely smaller.
        payload_bytes = (
            candidate_payload_bytes
            if len(candidate_payload_bytes) < len(original_bytes)
            else original_bytes
        )

        gzip_payload = gzip.compress(payload_bytes, compresslevel=9, mtime=0)

        if write_if_changed(gz_path, gzip_payload):
            stats.updated_gzip_files += 1

        stats.processed_files += 1
        stats.original_bytes += len(original_bytes)
        stats.payload_bytes += len(payload_bytes)
        stats.gzip_bytes += len(gzip_payload)

        next_cached_files[cache_key] = {
            "sourceHash": source_hash,
            "payloadSize": len(payload_bytes),
            "gzipSize": len(gzip_payload),
        }

    stats.removed_stale_gzip_files = remove_stale_gzip_files(data_dir)

    save_cache(
        cache_file,
        {
            "schemaVersion": CACHE_SCHEMA_VERSION,
            "signature": CACHE_SIGNATURE,
            "files": next_cached_files,
        },
    )

    return stats


def print_summary(stats: Stats, data_dir: Path) -> None:
    print(f"[ASSETS] Data directory: {data_dir}")
    print(f"[ASSETS] Frontend files processed: {stats.processed_files}")
    print(f"[ASSETS] Updated .gz files: {stats.updated_gzip_files}")
    print(f"[ASSETS] Reused unchanged .gz files: {stats.skipped_unchanged_files}")
    if stats.removed_stale_gzip_files > 0:
        print(f"[ASSETS] Removed stale .gz files: {stats.removed_stale_gzip_files}")

    if stats.original_bytes == 0:
        print("[ASSETS] No eligible frontend files found (HTML/CSS/JS).")
        return

    minified_gain = stats.original_bytes - stats.payload_bytes
    gzip_gain = stats.original_bytes - stats.gzip_bytes
    minified_pct = (minified_gain * 100.0) / stats.original_bytes
    gzip_pct = (gzip_gain * 100.0) / stats.original_bytes

    print(
        "[ASSETS] Size summary: "
        f"source={stats.original_bytes}B, "
        f"minified-payload={stats.payload_bytes}B ({minified_pct:.1f}% smaller), "
        f"gzip={stats.gzip_bytes}B ({gzip_pct:.1f}% smaller than source)"
    )


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Optimize SecureLock frontend assets")
    parser.add_argument(
        "--data-dir",
        default="data",
        help="Path to frontend data directory (default: data)",
    )
    parser.add_argument(
        "--cache-file",
        default="",
        help="Optional cache metadata file path for incremental preprocessing",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    data_dir = Path(args.data_dir).resolve()
    cache_file = (
        Path(args.cache_file).resolve()
        if str(args.cache_file).strip()
        else resolve_default_cache_file(data_dir)
    )

    if not data_dir.exists() or not data_dir.is_dir():
        print(f"[ASSETS][WARN] Data directory not found: {data_dir}")
        return 0

    stats = optimize_assets(data_dir, cache_file=cache_file)
    print_summary(stats, data_dir)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
