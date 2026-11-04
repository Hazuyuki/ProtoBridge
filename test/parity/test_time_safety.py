"""Run time-boundary checks in separate processes for every native time unit."""
import os
from pathlib import Path
import subprocess
import sys

import pytest

ROOT = Path(__file__).resolve().parents[2]
RUNNERS = sorted((ROOT / "build" / "utils").glob("ns3*-test-runner*"))
pytestmark = pytest.mark.skipif(not RUNNERS, reason="Build test-runner first")


@pytest.mark.parametrize("unit", range(10), ids=["Y", "D", "H", "MIN", "S", "MS", "US", "NS", "PS", "FS"])
def test_time_conversion_resolution_with_floating_point_traps(unit):
    env = {**os.environ, "PROTOBRIDGE_TIME_TEST_UNIT": str(unit), "PROTOBRIDGE_TIME_TEST_TRAPS": "1"}
    result = subprocess.run(
        [sys.executable, str(ROOT / "ns3"), "run",
         "test-runner --test-name=gpu-cluster-time-conversions --verbose", "--no-build"],
        cwd=ROOT, env=env, text=True, capture_output=True, timeout=30,
    )
    assert result.returncode == 0, result.stdout + result.stderr


def test_event_boundaries_and_resource_accounting_with_floating_point_traps():
    result = subprocess.run(
        [sys.executable, str(ROOT / "ns3"), "run",
         "test-runner --test-name=gpu-cluster-time-safety --verbose", "--no-build"],
        cwd=ROOT, env={**os.environ, "PROTOBRIDGE_TIME_TEST_TRAPS": "1"},
        text=True, capture_output=True, timeout=30,
    )
    assert result.returncode == 0, result.stdout + result.stderr
