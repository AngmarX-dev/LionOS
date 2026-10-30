#!/usr/bin/env python3
"""Smoke-check the upstream KernelCI core configuration and its stated scope."""

from __future__ import annotations

import sys
from pathlib import Path

import yaml


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit("usage: validate_scope.py <kernelci-core-checkout>")

    root = Path(sys.argv[1])
    readme = (root / "README.md").read_text(encoding="utf-8")
    runtimes = yaml.safe_load(
        (root / "config/core/runtime-configs.yaml").read_text(encoding="utf-8")
    )
    tests = yaml.safe_load(
        (root / "config/core/test-configs.yaml").read_text(encoding="utf-8")
    )

    assert "upstream Linux kernel" in readme, (
        "KernelCI core README no longer states its upstream Linux kernel scope"
    )
    assert "docker" in runtimes["runtimes"]
    assert any(
        value.get("lab_type", "").startswith("legacy.lava")
        for name, value in runtimes["runtimes"].items()
        if name.startswith("lab-")
    ), "Expected KernelCI's configured LAVA hardware-lab runtimes"
    assert isinstance(tests, dict) and "file_system_types" in tests

    print("KernelCI core repository checkout: PASS")
    print("KernelCI runtime/test configuration parsing: PASS")
    print("KernelCI LAVA runtime integration config: PASS")
    print("Native LionOS build/boot test in standard KernelCI Linux-kernel pipeline: NOT SUPPORTED")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
