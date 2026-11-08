#!/usr/bin/env python3
"""Bounded, checksum-verifying reader for NGS2SPK v1 containers.

TLG expands a packed module before its existing ELF parser/linker runs. The
resulting boot image remains ordinary native code; this is not a PS4-side
self-extracting PRX loader.
"""
from __future__ import annotations

import hashlib
import json
import lzma
import struct
import zlib

MAGIC = b"NGS2SPK\0"
VERSION = 1
HEADER = struct.Struct("<8sHHIQ")
CODEC_RAW = 0
CODEC_XZ = 1
CODEC_ZLIB = 2
MAX_MANIFEST = 16 * 1024 * 1024
MAX_ORIGINAL_SIZE = 512 * 1024 * 1024
MAX_LZMA_MEMORY = 256 * 1024 * 1024


class NGS2PackError(ValueError):
    """Invalid, truncated, oversized, or corrupted NGS2SPK input."""


def _integer(record: dict, key: str, minimum: int = 0) -> int:
    value = record.get(key)
    if type(value) is not int or value < minimum:
        raise NGS2PackError(f"invalid {key!r} in manifest")
    return value


def unpack_ngs2_container(blob: bytes) -> bytes:
    """Restore an NGS2SPK v1 container and verify every chunk plus the whole file."""
    if len(blob) < HEADER.size:
        raise NGS2PackError("container is shorter than its header")

    magic, version, flags, manifest_size, payload_size = HEADER.unpack_from(blob)
    if magic != MAGIC or version != VERSION or flags != 0:
        raise NGS2PackError("unsupported NGS2SPK signature, version, or flags")
    if manifest_size > MAX_MANIFEST:
        raise NGS2PackError("manifest exceeds safety limit")
    payload_start = HEADER.size + manifest_size
    if payload_start > len(blob) or payload_size != len(blob) - payload_start:
        raise NGS2PackError("manifest/payload lengths do not match container size")

    try:
        manifest = json.loads(blob[HEADER.size:payload_start].decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise NGS2PackError(f"invalid JSON manifest: {exc}") from exc
    if not isinstance(manifest, dict):
        raise NGS2PackError("manifest root must be an object")
    if manifest.get("format") != "NGS2 Segment Pack" or manifest.get("format_version") != VERSION:
        raise NGS2PackError("manifest format/version mismatch")

    original_size = _integer(manifest, "original_size", 1)
    if original_size > MAX_ORIGINAL_SIZE:
        raise NGS2PackError("original module exceeds 512 MiB safety limit")
    original_sha = manifest.get("original_sha256")
    if not isinstance(original_sha, str) or len(original_sha) != 64:
        raise NGS2PackError("invalid original SHA-256 in manifest")

    chunks = manifest.get("chunks")
    if not isinstance(chunks, list) or not chunks:
        raise NGS2PackError("manifest has no chunks")

    payload = blob[payload_start:]
    output = bytearray(original_size)
    file_cursor = payload_cursor = 0

    for index, record in enumerate(chunks):
        if not isinstance(record, dict):
            raise NGS2PackError(f"chunk {index}: record must be an object")
        file_offset = _integer(record, "file_offset")
        raw_size = _integer(record, "original_size", 1)
        packed_offset = _integer(record, "payload_offset")
        packed_size = _integer(record, "packed_size", 1)
        codec = _integer(record, "codec")
        expected_sha = record.get("sha256")

        if file_offset != file_cursor or raw_size > original_size - file_offset:
            raise NGS2PackError(f"chunk {index}: invalid/noncontiguous original range")
        if packed_offset != payload_cursor or packed_size > len(payload) - packed_offset:
            raise NGS2PackError(f"chunk {index}: invalid/noncontiguous payload range")
        if not isinstance(expected_sha, str) or len(expected_sha) != 64:
            raise NGS2PackError(f"chunk {index}: invalid SHA-256")

        packed = payload[packed_offset:packed_offset + packed_size]
        if codec == CODEC_RAW:
            raw = packed
        elif codec == CODEC_XZ:
            decoder = lzma.LZMADecompressor(format=lzma.FORMAT_XZ, memlimit=MAX_LZMA_MEMORY)
            try:
                raw = decoder.decompress(packed, max_length=raw_size + 1)
            except lzma.LZMAError as exc:
                raise NGS2PackError(f"chunk {index}: XZ decompression failed: {exc}") from exc
            if not decoder.eof or decoder.unused_data:
                raise NGS2PackError(f"chunk {index}: truncated XZ stream or trailing data")
        elif codec == CODEC_ZLIB:
            decoder = zlib.decompressobj()
            try:
                raw = decoder.decompress(packed, raw_size + 1)
                if len(raw) > raw_size or decoder.unconsumed_tail:
                    raise NGS2PackError(f"chunk {index}: zlib output exceeds declared size")
                raw += decoder.flush()
            except zlib.error as exc:
                raise NGS2PackError(f"chunk {index}: zlib decompression failed: {exc}") from exc
            if not decoder.eof or decoder.unused_data or decoder.unconsumed_tail:
                raise NGS2PackError(f"chunk {index}: invalid or trailing zlib data")
        else:
            raise NGS2PackError(f"chunk {index}: unknown codec {codec}")

        if len(raw) != raw_size:
            raise NGS2PackError(f"chunk {index}: expanded to {len(raw)} bytes, expected {raw_size}")
        if hashlib.sha256(raw).hexdigest() != expected_sha:
            raise NGS2PackError(f"chunk {index}: SHA-256 mismatch")

        output[file_offset:file_offset + raw_size] = raw
        file_cursor += raw_size
        payload_cursor += packed_size

    if file_cursor != original_size:
        raise NGS2PackError("chunks do not cover the complete original file")
    if payload_cursor != len(payload):
        raise NGS2PackError("unused or unreferenced bytes remain in payload")

    result = bytes(output)
    if hashlib.sha256(result).hexdigest() != original_sha:
        raise NGS2PackError("reconstructed module SHA-256 mismatch")
    return result
