#!/usr/bin/env python3
"""Validate the LionOS LAVA job with the upstream LAVA schema implementation."""

from __future__ import annotations

import sys
from pathlib import Path

import yaml
from lava_common.schemas import validate


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit("usage: validate_job.py <job.yaml>")

    job_path = Path(sys.argv[1])
    job = yaml.safe_load(job_path.read_text(encoding="utf-8"))
    if not isinstance(job, dict):
        raise SystemExit("LAVA job must be a YAML mapping")

    validate(job, strict=True)

    assert job["actions"][0]["boot"]["method"] == "uefi-menu"
    assert "interactive" in job["actions"][1]["test"]
    print("Upstream LAVA strict job-schema validation: PASS")
    print("LAVA interactive test schema: PASS")
    print("Live LAVA scheduling/device boot: NOT TESTED (no server/device registered)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
