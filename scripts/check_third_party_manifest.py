#!/usr/bin/env python3
"""Validate the LionOS third-party component register structure.

This is a metadata consistency check, not a legal license checker.
"""
from __future__ import annotations

import csv
import sys
from pathlib import Path
from urllib.parse import urlparse

ROOT = Path(__file__).resolve().parent.parent
MANIFEST = ROOT / "docs" / "THIRD_PARTY_COMPONENTS.csv"
REQUIRED = (
    "id",
    "name",
    "role",
    "upstream_url",
    "version",
    "revision",
    "license",
    "copyright",
    "source_paths",
    "distributed_in_iso",
    "distributed_in_usb",
    "local_modifications",
    "source_availability",
    "redistribution_obligations",
    "status",
    "notes",
)
STATUSES = {
    "PENDING_AUDIT",
    "BUILD_ONLY_NOT_SHIPPED",
    "APPROVED",
    "REJECTED",
}
UNKNOWN_MARKERS = {
    "",
    "UNKNOWN",
    "UNRECORDED",
    "UNPINNED_BY_CURRENT_BUILD",
    "NOT_RECORDED",
    "NOT_RECORDED",
    "NOT_APPLICABLE",
}


def fail(message: str) -> int:
    print(f"third-party manifest: ERROR: {message}", file=sys.stderr)
    return 1


def main() -> int:
    try:
        with MANIFEST.open("r", encoding="utf-8", newline="") as stream:
            reader = csv.DictReader(stream)
            if reader.fieldnames is None:
                return fail("missing CSV header")
            missing_columns = [name for name in REQUIRED if name not in reader.fieldnames]
            if missing_columns:
                return fail("missing columns: " + ", ".join(missing_columns))

            rows = list(reader)
    except (OSError, csv.Error) as error:
        return fail(f"cannot read {MANIFEST}: {error}")

    if not rows:
        return fail("manifest has no component records")

    seen_ids: set[str] = set()
    pending = 0
    approved = 0

    for line_number, row in enumerate(rows, start=2):
        component_id = (row.get("id") or "").strip()
        if not component_id:
            return fail(f"line {line_number}: empty component id")
        if component_id in seen_ids:
            return fail(f"line {line_number}: duplicate component id {component_id}")
        seen_ids.add(component_id)

        for field in REQUIRED:
            if not (row.get(field) or "").strip():
                return fail(f"line {line_number}: empty required field {field}")

        status = row["status"].strip()
        if status not in STATUSES:
            return fail(f"line {line_number}: unsupported status {status}")

        for field in ("distributed_in_iso", "distributed_in_usb"):
            if row[field].strip() not in {"YES", "NO", "N/A"}:
                return fail(f"line {line_number}: {field} must be YES, NO, or N/A")

        upstream = row["upstream_url"].strip()
        if upstream not in {"UNKNOWN", "NOT_APPLICABLE"}:
            urls = upstream.split(";")
            for value in urls:
                parsed = urlparse(value.strip())
                if parsed.scheme != "https" or not parsed.netloc:
                    return fail(f"line {line_number}: invalid HTTPS upstream URL {value!r}")

        if status == "APPROVED":
            evidence_fields = (
                "upstream_url",
                "version",
                "revision",
                "license",
                "copyright",
                "source_paths",
                "source_availability",
                "redistribution_obligations",
            )
            for field in evidence_fields:
                if row[field].strip().upper() in UNKNOWN_MARKERS:
                    return fail(
                        f"line {line_number}: APPROVED row has unresolved {field}"
                    )
            if row["source_availability"].strip().upper() in {
                "NOT_RECORDED",
                "NOT_YET_RECORDED",
            }:
                return fail(f"line {line_number}: APPROVED row lacks source availability")
            if row["redistribution_obligations"].strip().upper() in {
                "UNKNOWN",
                "UNRECORDED",
                "NOT_RECORDED",
            }:
                return fail(f"line {line_number}: APPROVED row lacks obligations")
            approved += 1
        elif status == "PENDING_AUDIT":
            pending += 1

        if status == "BUILD_ONLY_NOT_SHIPPED" and (
            row["distributed_in_iso"].strip() != "NO"
            or row["distributed_in_usb"].strip() != "NO"
        ):
            return fail(
                f"line {line_number}: build-only component cannot be marked shipped"
            )

    print(
        "third-party manifest: PASS "
        f"({len(rows)} records; {approved} approved; {pending} pending audit)"
    )
    print("This validates metadata structure only; it is not legal approval.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
