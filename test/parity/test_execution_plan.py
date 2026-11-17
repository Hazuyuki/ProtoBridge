"""The reported execution plan must describe the schedule that ran."""

from pathlib import Path
import subprocess

import pytest


ROOT = Path(__file__).resolve().parents[2]
BINS = sorted((ROOT / "build/scratch").glob("*gpu-cluster-sim*"))
BIN = next((p for p in BINS if p.is_file() and "debug" in p.name),
           next((p for p in BINS if p.is_file()), None))
pytestmark = pytest.mark.skipif(BIN is None, reason="Build gpu-cluster-sim first")


def run(**parameters):
    args = dict(numGpus=4, topology="switched", bandwidth=100, latency=10,
                bulkChunkSize=65536, startupDelay=1000, startupPerGpuNs=0,
                fecEnabled=0, llrEnabled=0, ber=0, collective="allreduce",
                algorithm="auto", forceProtocol=0, dataSize=4096)
    args.update(parameters)
    result = subprocess.run([str(BIN), *[f"--{key}={value}" for key, value in args.items()]],
                            cwd=ROOT, capture_output=True, text=True, timeout=60)
    assert result.returncode == 0, result.stdout + result.stderr
    block = result.stdout.split("RESULT_START", 1)[1].split("RESULT_END", 1)[0]
    fields = dict(line.split("=", 1) for line in block.splitlines() if "=" in line)
    assert fields["status"] == "complete"
    assert fields["executionPlanVersion"] == "1"
    assert fields["planThreadsPerChannel"] == "-1"
    return fields


@pytest.mark.parametrize("size,algorithm,protocol,policy", [
    (4096, "tree", "LL", "size_threshold"),
    (8388608, "nvls", "SIMPLE", "algorithm_constraint"),
])
def test_auto_reports_algorithm_and_protocol_constraints(size, algorithm, protocol, policy):
    result = run(dataSize=size)
    assert result["selectedAlgorithm"] == algorithm
    assert result["selectedProtocol"] == protocol
    assert result["algorithmSelectionPolicy"] == "simulator_heuristic"
    assert result["protocolSelectionPolicy"] == policy


def test_forced_ll_filters_out_simple_only_nvls():
    result = run(dataSize=8388608, forceProtocol=1)
    assert result["selectedAlgorithm"] == "tree"
    assert result["selectedProtocol"] == "LL"
    assert result["protocolSelectionPolicy"] == "forced"


def test_reduce_scatter_reports_its_implemented_ring():
    result = run(collective="reducescatter", dataSize=8388608)
    assert result["selectedAlgorithm"] == "ring"
    assert result["planImplementation"] == "ns3::RingReduceScatter"


def test_nvls_allgather_uses_simple_even_when_its_upload_chunk_is_small():
    result = run(collective="allgather", numGpus=8, dataSize=8388608)
    assert result["selectedAlgorithm"] == "nvls"
    assert result["selectedProtocol"] == "SIMPLE"


def test_parallel_ring_reports_resolved_channels():
    result = run(algorithm="ncclring", numNodes=1, gpusPerNode=4,
                 ringChannels=2, dataSize=65536, forceProtocol=3)
    assert result["selectedAlgorithm"] == "ncclring"
    assert result["planChannels"] == "2"
    assert result["channelModel"] == "rail_rings"


def test_switched_alltoall_reports_direct_exchange():
    result = run(collective="alltoall", algorithm="ring", dataSize=65536)
    assert result["selectedAlgorithm"] == "direct"
    assert result["planImplementation"] == "ns3::AlltoAllInjector"
