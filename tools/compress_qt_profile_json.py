#!/usr/bin/env python3
"""Create Qt qCompress-compatible profile JSON payloads.

The Ksword UI reads profile data with Qt's qUncompress(), whose on-disk format
is not a raw zlib stream.  qCompress() stores four big-endian bytes containing
the uncompressed length and then appends the zlib-compressed payload.  This
script mirrors that layout at build time so the application can keep using Qt
for runtime decompression without shipping a custom decompressor.
"""

from __future__ import annotations

import argparse
import fnmatch
import os
from pathlib import Path
import struct
import sys
import zlib


def qt_compress_bytes(data: bytes, level: int) -> bytes:
    """Return a QByteArray layout compatible with Qt qUncompress().

    Args:
        data: Raw JSON bytes read from the source profile.
        level: zlib compression level in the range accepted by zlib.compress().

    Returns:
        Four-byte big-endian uncompressed size followed by the compressed zlib
        stream.  The caller is responsible for writing the bytes to disk.
    """

    return struct.pack(">I", len(data)) + zlib.compress(data, level)


def compress_file(source_path: Path, output_path: Path, level: int) -> None:
    """Compress one JSON file to one Qt-compatible .qz file.

    Args:
        source_path: Existing source JSON file path.
        output_path: Destination path, normally ending with ".json.qz".
        level: zlib compression level.

    Returns:
        None.  Raises OSError/ValueError on invalid input or write failures.
    """

    if not source_path.is_file():
        raise FileNotFoundError(f"source JSON does not exist: {source_path}")

    raw_data = source_path.read_bytes()
    compressed_data = qt_compress_bytes(raw_data, level)

    # Verify the stream immediately.  Build-time validation is cheap and avoids
    # discovering a bad payload only after the release package is copied.
    restored_data = zlib.decompress(compressed_data[4:])
    if restored_data != raw_data:
        raise ValueError(f"verification failed after compression: {source_path}")

    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_bytes(compressed_data)


def compress_directory(source_dir: Path, output_dir: Path, pattern: str, level: int) -> int:
    """Compress all matching files from one directory into another directory.

    Args:
        source_dir: Directory containing source JSON files.
        output_dir: Directory receiving files named "<source>.qz".
        pattern: fnmatch-style filename pattern, for example "*.json".
        level: zlib compression level.

    Returns:
        Number of files compressed.
    """

    if not source_dir.is_dir():
        raise FileNotFoundError(f"source directory does not exist: {source_dir}")

    matched_count = 0
    for child in sorted(source_dir.iterdir(), key=lambda item: item.name.lower()):
        if not child.is_file() or not fnmatch.fnmatch(child.name, pattern):
            continue

        compress_file(child, output_dir / f"{child.name}.qz", level)
        matched_count += 1

    return matched_count


def build_argument_parser() -> argparse.ArgumentParser:
    """Build and return the command-line parser used by main().

    Args:
        None.

    Returns:
        Configured ArgumentParser instance.
    """

    parser = argparse.ArgumentParser(
        description="Compress Ksword profile JSON files into Qt qCompress-compatible .json.qz payloads."
    )
    parser.add_argument("--level", type=int, default=9, help="zlib compression level, default: 9")

    mode_group = parser.add_mutually_exclusive_group(required=True)
    mode_group.add_argument("--file", type=Path, help="single source JSON file")
    mode_group.add_argument("--dir", type=Path, help="source directory for batch compression")

    parser.add_argument("--out", type=Path, help="single output .qz path, required with --file")
    parser.add_argument("--out-dir", type=Path, help="output directory, required with --dir")
    parser.add_argument("--pattern", default="*.json", help="directory filename pattern, default: *.json")
    return parser


def main(argv: list[str]) -> int:
    """Run the compressor from command-line arguments.

    Args:
        argv: Command-line arguments excluding the executable name.

    Returns:
        Process exit code: 0 on success, non-zero on validation or IO failure.
    """

    parser = build_argument_parser()
    args = parser.parse_args(argv)

    if args.level < 0 or args.level > 9:
        parser.error("--level must be between 0 and 9")

    try:
        if args.file is not None:
            if args.out is None:
                parser.error("--out is required with --file")
            compress_file(args.file, args.out, args.level)
            print(f"compressed: {args.file} -> {args.out}")
            return 0

        if args.out_dir is None:
            parser.error("--out-dir is required with --dir")

        count = compress_directory(args.dir, args.out_dir, args.pattern, args.level)
        print(f"compressed {count} file(s): {args.dir} -> {args.out_dir}")
        return 0
    except Exception as exc:  # noqa: BLE001 - command-line tool should report any build-blocking failure.
        print(f"compress_qt_profile_json.py: error: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
