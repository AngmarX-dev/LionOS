#!/usr/bin/env python3
"""Load the example with Labgrid and verify the serial resource/driver binding.

The SerialDriver is deliberately not activated: this check must not open a real
serial port or require lab equipment on a GitHub-hosted runner.
"""

from __future__ import annotations

import sys
from pathlib import Path

from labgrid import Environment
from labgrid.driver import SerialDriver
from labgrid.resource import RawSerialPort


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit("usage: validate_environment.py <environment.yaml>")

    env = Environment(config_file=str(Path(sys.argv[1])))
    try:
        target = env.get_target("loq15iax9")
        assert target is not None, "Labgrid target 'loq15iax9' was not loaded"

        raw_port = target.get_resource(RawSerialPort, wait_avail=False)
        assert raw_port.port == "/dev/ttyUSB0"
        assert raw_port.speed == 115200

        driver = target.get_driver(SerialDriver, activate=False)
        bound = driver.get_bound_resources()
        assert len(bound) == 1 and raw_port in bound, (
            "SerialDriver is not bound to the configured RawSerialPort"
        )

        print("Upstream Labgrid environment parsing: PASS")
        print("RawSerialPort resource creation: PASS")
        print("SerialDriver binding (not activated): PASS")
        print("Physical serial connection / LionOS console: NOT TESTED")
    finally:
        env.cleanup()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
