#!/usr/bin/env python3
"""
Dictionary-encode NDJSON rows into a single wide table with all columns.

Unlike json_dict_encode.py (which groups rows by schema), this script creates
one wide table where every unique JSON path is a column. Rows that don't have
a given key get null in that column.

Output binary format (all sections independently ZSTD-compressed):
    keys:   newline-joined key strings
    values: newline-joined value strings
    rows:   Avro container with one field per key, int value IDs (nullable)

Usage:
    python json_single_table.py INPUT.ndjson OUTPUT.bin [--zstd-level LEVEL]
"""

import argparse
import io
import json
import struct
import sys
from collections import OrderedDict
from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple

import fastavro


def flatten_json(obj: Any, prefix: str = "") -> List[Tuple[str, str]]:
    """Recursively flatten a JSON value into (path, json_leaf_value) pairs."""
    items: List[Tuple[str, str]] = []
    if isinstance(obj, dict):
        for key, val in obj.items():
            full = f"{prefix}.{key}" if prefix else key
            items.extend(flatten_json(val, full))
    elif isinstance(obj, list):
        for idx, val in enumerate(obj):
            full = f"{prefix}[{idx}]"
            items.extend(flatten_json(val, full))
    else:
        items.append((prefix, str(obj)))
    return items


def _pick_avro_type(max_val: int) -> str:
    """Pick the smallest Avro integer type that fits max_val."""
    if max_val <= 0x7FFFFFFF:
        return "int"
    return "long"


def convert(input_path: Path, output_path: Path, zstd_level: Optional[int] = None) -> None:
    key_dict: OrderedDict[str, int] = OrderedDict()
    val_dict: OrderedDict[str, int] = OrderedDict()

    # Each row: dict mapping key_id -> val_id (sparse).
    rows: List[Dict[int, int]] = []

    def get_key_id(k: str) -> int:
        if k not in key_dict:
            key_dict[k] = len(key_dict)
        return key_dict[k]

    def get_val_id(v: str) -> int:
        if v not in val_dict:
            val_dict[v] = len(val_dict)
        return val_dict[v]

    with input_path.open("r", encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            obj = json.loads(line)
            flat = flatten_json(obj)
            row: Dict[int, int] = {}
            for k, v in flat:
                kid = get_key_id(k)
                vid = get_val_id(v)
                row[kid] = vid
            rows.append(row)

    keys_list = list(key_dict.keys())
    vals_list = list(val_dict.keys())
    num_keys = len(keys_list)
    num_rows = len(rows)
    num_vals = len(vals_list)

    # Build Avro schema: each key becomes a nullable field.
    # Determine max value per column for type selection.
    col_max: List[int] = [0] * num_keys
    for row in rows:
        for kid, vid in row.items():
            if vid > col_max[kid]:
                col_max[kid] = vid

    avro_fields = []
    for kid in range(num_keys):
        base_type = _pick_avro_type(col_max[kid])
        avro_fields.append({
            "name": f"c{kid}",
            "type": ["null", base_type],
            "default": None,
        })

    avro_schema = fastavro.parse_schema({
        "type": "record",
        "name": "Row",
        "fields": avro_fields,
    })

    # Build row dicts for Avro.
    avro_records: List[Dict[str, Optional[int]]] = []
    for row in rows:
        rec: Dict[str, Optional[int]] = {}
        for kid in range(num_keys):
            vid = row.get(kid)
            rec[f"c{kid}"] = vid
        avro_records.append(rec)

    # Compute fill statistics before serialization.
    null_count = sum(
        1 for rec in avro_records for kid in range(num_keys) if rec.get(f"c{kid}") is None
    )
    total_cells = num_rows * num_keys

    # Serialize keys and values as newline-joined strings.
    def serialize_strings(strings: List[str]) -> bytes:
        parts: List[bytes] = [struct.pack("<I", len(strings))]
        for s in strings:
            parts.append(s.encode("utf-8"))
        return b"\n".join(parts)

    keys_raw = serialize_strings(keys_list)
    values_raw = serialize_strings(vals_list)

    # Serialize rows as Avro container.
    avro_buf = io.BytesIO()
    fastavro.writer(avro_buf, avro_schema, avro_records)
    rows_raw = avro_buf.getvalue()

    section_data = [
        ("keys", keys_raw),
        ("values", values_raw),
        ("rows", rows_raw),
    ]

    compressor = None
    if zstd_level is not None:
        import zstandard as zstd
        compressor = zstd.ZstdCompressor(level=zstd_level)

    compressed_sections: List[Tuple[str, bytes, int]] = []
    for name, raw in section_data:
        if compressor is not None:
            compressed = compressor.compress(raw)
        else:
            compressed = raw
        compressed_sections.append((name, compressed, len(raw)))

    # Output format: <uint32 num_sections> [<uint32 name_len> <name> <uint64 data_len> <data>]...
    with output_path.open("wb") as out:
        out.write(struct.pack("<I", len(compressed_sections)))
        for name, data, _ in compressed_sections:
            name_b = name.encode("utf-8")
            out.write(struct.pack("<I", len(name_b)))
            out.write(name_b)
            out.write(struct.pack("<Q", len(data)))
            out.write(data)

    file_size = output_path.stat().st_size
    total_raw = sum(raw_size for _, _, raw_size in compressed_sections)
    total_compressed = sum(len(data) for _, data, _ in compressed_sections)

    # Statistics.
    fill_rate = (total_cells - null_count) / total_cells * 100 if total_cells > 0 else 0

    print(f"Rows: {num_rows}, Keys: {num_keys}, Values: {num_vals}", file=sys.stderr)
    print(f"Table: {num_rows} x {num_keys} = {total_cells:,} cells, "
          f"{null_count:,} nulls ({100 - fill_rate:.1f}%), "
          f"fill rate {fill_rate:.1f}%", file=sys.stderr)
    print(f"\n{'Section':<12} {'Raw':>12} {'Compressed':>12} {'Ratio':>8}", file=sys.stderr)
    print("-" * 48, file=sys.stderr)
    for name, data, raw_size in compressed_sections:
        comp_size = len(data)
        ratio = raw_size / comp_size if comp_size > 0 else 0
        print(f"{name:<12} {raw_size:>12,} {comp_size:>12,} {ratio:>7.2f}x", file=sys.stderr)
    print("-" * 48, file=sys.stderr)
    ratio = total_raw / total_compressed if total_compressed > 0 else 0
    print(f"{'TOTAL':<12} {total_raw:>12,} {total_compressed:>12,} {ratio:>7.2f}x", file=sys.stderr)
    input_size = input_path.stat().st_size
    print(f"\nInput file:  {input_size:,} bytes", file=sys.stderr)
    print(f"Output file: {file_size:,} bytes", file=sys.stderr)
    if file_size > 0:
        print(f"Input/Output ratio: {input_size / file_size:.2f}x", file=sys.stderr)


def main() -> None:
    ap = argparse.ArgumentParser(
        description="Dictionary-encode NDJSON into a single wide Avro table + ZSTD.",
    )
    ap.add_argument("input", type=Path, help="Input NDJSON file")
    ap.add_argument("output", type=Path, help="Output binary file")
    ap.add_argument(
        "--zstd-level",
        type=int,
        default=None,
        metavar="LEVEL",
        help="ZSTD compression level (1..22). If omitted, no compression.",
    )
    args = ap.parse_args()

    if not args.input.exists():
        ap.error(f"input file does not exist: {args.input}")

    convert(args.input, args.output, zstd_level=args.zstd_level)


if __name__ == "__main__":
    main()
