"""Native IBGDA scheduling, mode compatibility and completion semantics."""
from pathlib import Path
import csv
import resource
import subprocess

import pytest

ROOT = Path(__file__).resolve().parents[2]
BINARIES = [p for p in (ROOT / "build/scratch").glob("*gpu-cluster-sim*")
            if p.is_file() and p.stat().st_mode & 0o111]
pytestmark = pytest.mark.skipif(not BINARIES, reason="Build gpu-cluster-sim first.")


def run(*args, success=True):
    def no_core():
        resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    result = subprocess.run([str(BINARIES[0]), *args], cwd=ROOT, text=True,
                            capture_output=True, timeout=30, preexec_fn=no_core)
    output = result.stdout + result.stderr
    if success:
        assert result.returncode == 0, output[-3000:]
    else:
        assert result.returncode != 0, output[-3000:]
        return output
    fields = dict(line.split("=", 1) for line in result.stdout.splitlines()
                  if "=" in line and not line.startswith("CONFIG:"))
    assert fields.get("status") == "complete", output[-3000:]
    return fields


BASE = ["--numNodes=2", "--gpusPerNode=2", "--interNodeTopology=host",
        "--interNodeFabricType=RoCE", "--collective=p2p", "--rootRank=1"]


def test_legacy_default_matches_explicit_mode_and_ignores_ibgda_timings():
    args = ["--numNodes=2", "--gpusPerNode=2", "--collective=allreduce",
            "--algorithm=hierarchical", "--dataSize=65536", "--interNodeTopology=host"]
    default = run(*args)
    explicit = run(*args, "--rdmaSubmission=startup", "--ibgdaWqeBuildNs=100000")
    assert default["simTimeNs"] == explicit["simTimeNs"]
    assert default["rdmaSubmission"] == explicit["rdmaSubmission"] == "startup"
    assert "ibgdaSubmittedWqes" not in default


@pytest.mark.parametrize("iterations,depth,batch,cq,window", [
    (1, 16, 16, 16, 0), (17, 3, 3, 3, 0), (17, 3, 3, 3, 2),
    (129, 16, 8, 16, 0), (257, 128, 32, 100, 8),
])
def test_wr_depth_fragment_acks_and_partial_groups(iterations, depth, batch, cq, window):
    fields = run(*BASE, "--rdmaSubmission=ibgda", "--dataSize=9000",
                 f"--p2pIterations={iterations}", f"--ibgdaQueueDepth={depth}",
                 f"--ibgdaDoorbellBatch={batch}", f"--ibgdaCqModeration={cq}",
                 f"--soTxWindow={window}", "--ibgdaWqeBuildNs=100")
    assert int(fields["ibgdaSubmittedWqes"]) == iterations
    assert int(fields["ibgdaCompletedWqes"]) == iterations
    assert int(fields["ibgdaStartupSamples"]) == iterations
    assert int(fields["ibgdaFailedWqes"]) == 0
    assert int(fields["ibgdaPeakQpOutstanding"]) <= depth
    assert int(fields["simTimeNs"]) >= int(fields["dataDeliveryDurationNs"])


def test_cq_observation_changes_completion_without_delaying_first_wire(tmp_path):
    trace = tmp_path / "trace.csv"
    first = run(*BASE, "--rdmaSubmission=ibgda", "--dataSize=64",
                "--ibgdaWqeBuildNs=100", "--ibgdaDoorbellNs=20",
                "--ibgdaWqeFetchNs=30", "--ibgdaDmaReadNs=40")
    second = run(*BASE, "--rdmaSubmission=ibgda", "--dataSize=64",
                 "--ibgdaWqeBuildNs=100", "--ibgdaDoorbellNs=20",
                 "--ibgdaWqeFetchNs=30", "--ibgdaDmaReadNs=40",
                 "--ibgdaCqWriteNs=1000", "--ibgdaGpuPollNs=2000",
                 f"--ibgdaTraceFile={trace}")
    assert first["dataDeliveryDurationNs"] == second["dataDeliveryDurationNs"]
    assert first["ibgdaMeanStartupNs"] == second["ibgdaMeanStartupNs"] == "190"
    assert int(second["simTimeNs"]) - int(first["simTimeNs"]) == 3000
    with trace.open() as stream:
        rows = list(csv.DictReader(stream))
    assert len(rows) == 1
    row = rows[0]
    assert int(row["firstWireNs"]) - int(row["readyNs"]) == 190
    assert int(row["observedNs"]) - int(row["ackedNs"]) == 3000


def test_profile_selects_ibgda_and_prevents_double_counted_stencil_startup():
    fields = run("--protocolConfig=configs/protocol_configs/ibgda-p2p.cfg",
                 "--numNodes=2", "--gpusPerNode=2", "--dataSize=64")
    assert fields["rdmaSubmission"] == "ibgda"
    assert fields["ibgdaSubmittedWqes"] == fields["ibgdaCompletedWqes"] == "1"
    # Profile settings take precedence over the CLI, as for other hardware knobs.
    overridden = run("--protocolConfig=configs/protocol_configs/ibgda-p2p.cfg",
                     "--numNodes=2", "--gpusPerNode=2", "--dataSize=64",
                     "--rdmaSubmission=startup")
    assert overridden["simTimeNs"] == fields["simTimeNs"]
    assert overridden["rdmaSubmission"] == "ibgda"


def test_gpu_parallelism_with_separate_qps_reduces_submission_wait():
    args = [*BASE, "--rdmaSubmission=ibgda", "--dataSize=2", "--p2pIterations=64",
            "--ibgdaWqeBuildNs=10000"]
    serial = run(*args)
    parallel = run(*args, "--ibgdaProducers=4", "--ibgdaQpsPerPeer=4")
    assert int(parallel["simTimeNs"]) < int(serial["simTimeNs"])
    assert float(parallel["ibgdaMeanStartupNs"]) < float(serial["ibgdaMeanStartupNs"])


def test_per_qp_rate_cap_uses_physical_qps_instead_of_logical_otp_flow():
    args = [*BASE, "--rdmaSubmission=ibgda", "--dataSize=65536", "--p2pIterations=32",
            "--perQpCeilingGbps=10", "--interNodeLatency=0", "--switchCutThroughDelay=649"]
    serial = run(*args)
    parallel = run(*args, "--ibgdaProducers=4", "--ibgdaQpsPerPeer=4")
    assert int(parallel["simTimeNs"]) < 0.6 * int(serial["simTimeNs"])


@pytest.mark.parametrize("control", ["loss", "pfc"])
def test_packet_recovery_and_congestion_preserve_wr_completion(control):
    args = ["--numNodes=2", "--gpusPerNode=2", "--interNodeTopology=host",
            "--collective=incast", "--rootRank=2", "--rdmaSubmission=ibgda",
            "--p2pIterations=32", "--dataSize=8192", "--interNodeLatency=0",
            "--switchCutThroughDelay=649", "--ibgdaProducers=4",
            "--ibgdaQpsPerPeer=4", "--ibgdaQueueDepth=16", "--ibgdaDmaReadNs=50"]
    if control == "loss":
        args += ["--llrEnabled=1", "--llrMode=sack", "--packetLossRate=0.01",
                 "--llrTimeoutRetransmit=1", "--llrRetryTimeout=200000", "--llrRetryLimit=50"]
    else:
        args += ["--dcqcn=1", "--ecnThreshold=1", "--switchPfcMode=3",
                 "--pfcPauseThreshold=4", "--pfcResumeOffset=1"]
    fields = run(*args)
    assert fields["ibgdaSubmittedWqes"] == fields["ibgdaCompletedWqes"] == "64"
    assert fields["ibgdaFailedWqes"] == "0"
    if control == "loss":
        assert int(fields["retransmittedPackets"]) > 0
        assert int(fields["permanentLosses"]) == 0
    else:
        assert int(fields["pfcPausesSent"]) > 0
        assert int(fields["cnpPacketsReceived"]) > 0


@pytest.mark.parametrize("flags,message", [
    (["--rdmaSubmission=bad"], "rdmaSubmission"),
    (["--rdmaSubmission=ibgda", "--ibgdaQueueDepth=0"], "positive"),
    (["--rdmaSubmission=ibgda", "--ibgdaQueueDepth=4", "--ibgdaDoorbellBatch=8"], "queueDepth"),
    (["--rdmaSubmission=ibgda", "--ibgdaDoorbellBatch=2", "--ibgdaBatchTimeoutNs=0"], "timeout"),
    (["--rdmaSubmission=ibgda", "--ibgdaPcieReadGbps=-1"], "bandwidth"),
])
def test_invalid_settings_fail_before_simulation(flags, message):
    output = run(*BASE, *flags, success=False)
    assert message.lower() in output.lower()
