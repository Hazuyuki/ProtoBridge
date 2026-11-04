"""Controlled native checks separating request initiation and payload memory."""
import csv

import pytest

from test_ibgda import run, pytestmark  # noqa: F401

BASE = ["--numNodes=2", "--gpusPerNode=2", "--interNodeTopology=host",
        "--interNodeFabricType=RoCE", "--interNodeLatency=0",
        "--switchCutThroughDelay=649", "--collective=p2p", "--rootRank=1",
        "--rdmaSubmission=detailed"]


@pytest.mark.parametrize("actor", ["cpu", "gpu"])
@pytest.mark.parametrize("path", ["host", "gpudirect", "staged"])
def test_all_initiator_and_payload_path_combinations(actor, path):
    fields = run(*BASE, f"--rdmaInitiator={actor}", f"--rdmaDataPath={path}",
                 "--dataSize=9000", "--p2pIterations=3", "--rdmaStagingChunkBytes=1024",
                 "--rdmaStagingBufferBytes=2048", "--rdmaQueueDepth=3", "--rdmaCqModeration=3")
    assert fields["rdmaInitiator"] == actor
    assert fields["rdmaDataPath"] == path
    assert fields["rdmaSubmittedRequests"] == fields["rdmaCompletedRequests"] == "3"
    assert fields["rdmaSubmittedWqes"] == fields["rdmaCompletedWqes"] == ("27" if path == "staged" else "3")
    assert int(fields["rdmaSourceD2hBytes"]) == (27000 if path == "staged" else 0)
    assert int(fields["rdmaDestinationH2dBytes"]) == (27000 if path == "staged" else 0)
    assert int(fields["rdmaPeakQpOutstanding"]) <= 3


def test_cpu_and_gpu_have_independent_submission_costs():
    args = [*BASE, "--rdmaDataPath=gpudirect", "--dataSize=2",
            "--rdmaCpuWqeBuildNs=100", "--rdmaGpuWqeBuildNs=1000"]
    cpu, gpu = run(*args, "--rdmaInitiator=cpu"), run(*args, "--rdmaInitiator=gpu")
    assert float(cpu["rdmaMeanStartupNs"]) == 100
    assert float(gpu["rdmaMeanStartupNs"]) == 1000
    assert int(gpu["simTimeNs"]) - int(cpu["simTimeNs"]) == 900


def test_host_payload_ignores_hbm_bandwidth_and_gpu_payload_uses_it():
    args = [*BASE, "--rdmaInitiator=cpu", "--dataSize=8192"]
    fast = run(*args, "--rdmaDataPath=host", "--rdmaHbmReadGbps=1000")
    slow = run(*args, "--rdmaDataPath=host", "--rdmaHbmReadGbps=1")
    assert fast["simTimeNs"] == slow["simTimeNs"]
    assert fast["rdmaHbmReadBytes"] == fast["rdmaHbmWriteBytes"] == "0"
    gpu = run(*args, "--rdmaDataPath=gpudirect", "--rdmaHbmReadGbps=1")
    assert int(gpu["simTimeNs"]) > int(slow["simTimeNs"])
    assert gpu["rdmaHbmReadBytes"] == gpu["rdmaHbmWriteBytes"] == "8192"


def test_staging_adds_actual_copies_and_separates_transport_from_gpu_readiness():
    args = [*BASE, "--rdmaInitiator=cpu", "--dataSize=2"]
    direct = run(*args, "--rdmaDataPath=gpudirect")
    staged = run(*args, "--rdmaDataPath=staged", "--rdmaD2hNs=1000", "--rdmaH2dNs=10000")
    assert float(staged["rdmaMeanStartupNs"]) - float(direct["rdmaMeanStartupNs"]) == 1000
    assert int(staged["dataDeliveryDurationNs"]) - int(direct["dataDeliveryDurationNs"]) == 11000
    assert int(staged["rdmaCompletionObservedNs"]) < int(staged["rdmaDataReadyNs"])
    assert staged["rdmaSourceD2hBytes"] == staged["rdmaDestinationH2dBytes"] == "2"
    assert int(staged["simTimeNs"]) >= int(staged["rdmaDataReadyNs"])


def test_staging_capacity_backpressures_and_two_buffers_allow_overlap(tmp_path):
    args = [*BASE, "--rdmaInitiator=cpu", "--rdmaDataPath=staged", "--dataSize=3072",
            "--rdmaStagingChunkBytes=512", "--rdmaD2hNs=1000", "--rdmaH2dNs=1000"]
    single = run(*args, "--rdmaStagingBufferBytes=512")
    trace = tmp_path / "rdma.csv"
    double = run(*args, "--rdmaStagingBufferBytes=1024", f"--rdmaTraceFile={trace}")
    assert int(double["simTimeNs"]) < int(single["simTimeNs"])
    for fields, capacity in [(single,512), (double,1024)]:
        assert int(fields["rdmaSourceStagingPeakBytes"]) <= capacity
        assert int(fields["rdmaDestinationStagingPeakBytes"]) <= capacity
        assert fields["rdmaCompletedRequests"] == "1"
        assert fields["rdmaCompletedWqes"] == "6"
    with trace.open() as stream:
        rows = list(csv.DictReader(stream))
    assert len(rows) == 6
    assert sum(int(row["bytes"]) for row in rows) == 3072
    assert len({row["requestId"] for row in rows}) == 1
    assert {int(row["offset"]) for row in rows} == set(range(0,3072,512))
    for row in rows:
        assert int(row["submittedNs"]) >= int(row["sourceStagedNs"])
        assert int(row["reservedNs"]) >= int(row["submittedNs"])


def test_gpu_to_cpu_handoff_is_independent_of_the_payload_copy():
    args = [*BASE, "--rdmaInitiator=cpu", "--dataSize=2", "--rdmaCpuHandoffNs=1000"]
    host = run(*args, "--rdmaDataPath=host")
    direct = run(*args, "--rdmaDataPath=gpudirect")
    assert float(host["rdmaMeanStartupNs"]) == 0
    assert float(direct["rdmaMeanStartupNs"]) == 1000


def test_old_ibgda_alias_and_generic_gpu_direct_configuration_match():
    args = ["--numNodes=2", "--gpusPerNode=2", "--collective=p2p", "--rootRank=1",
            "--interNodeTopology=host", "--dataSize=9000", "--p2pIterations=17"]
    old = run(*args, "--rdmaSubmission=ibgda", "--ibgdaWqeBuildNs=100",
              "--ibgdaProducers=4", "--ibgdaQpsPerPeer=2")
    new = run(*args, "--rdmaSubmission=detailed", "--rdmaInitiator=gpu",
              "--rdmaDataPath=gpudirect", "--rdmaGpuWqeBuildNs=100",
              "--rdmaGpuProducers=4", "--rdmaQpsPerPeer=2")
    assert old["simTimeNs"] == new["simTimeNs"]
    assert old["rdmaMeanStartupNs"] == new["rdmaMeanStartupNs"]


@pytest.mark.parametrize("actor,path", [("cpu", "host"), ("cpu", "staged"),
                                         ("cpu", "gpudirect"), ("gpu", "gpudirect")])
def test_hierarchical_collectives_wait_for_payload_and_request_drain(actor, path):
    fields = run("--numNodes=2", "--gpusPerNode=2", "--interNodeTopology=host",
                 "--interNodeFabricType=RoCE", "--interNodeLatency=0",
                 "--collective=allreduce", "--algorithm=hierarchical", "--dataSize=8192",
                 "--rdmaSubmission=detailed", f"--rdmaInitiator={actor}",
                 f"--rdmaDataPath={path}", "--rdmaStagingChunkBytes=1024",
                 "--rdmaStagingBufferBytes=2048", "--rdmaD2hNs=1000", "--rdmaH2dNs=10000")
    assert int(fields["rdmaCompletedRequests"]) > 0
    assert fields["rdmaCompletedRequests"] == fields["rdmaSubmittedRequests"]
    assert fields["rdmaCompletedWqes"] == fields["rdmaSubmittedWqes"]
    assert int(fields["simTimeNs"]) >= int(fields["dataDeliveryDurationNs"])
    assert int(fields["simTimeNs"]) >= int(fields["rdmaDataReadyNs"])


def test_staged_writes_recover_loss_while_waiting_for_receive_buffers():
    fields = run(*BASE, "--rdmaInitiator=cpu", "--rdmaDataPath=staged",
                 "--dataSize=8192", "--p2pIterations=32", "--rdmaStagingChunkBytes=4096",
                 "--rdmaStagingBufferBytes=8192", "--rdmaH2dNs=50000",
                 "--llrEnabled=1", "--llrMode=sack", "--packetLossRate=0.01",
                 "--llrTimeoutRetransmit=1", "--llrRetryTimeout=200000", "--llrRetryLimit=50")
    assert fields["rdmaCompletedRequests"] == "32"
    assert fields["rdmaCompletedWqes"] == "64"
    assert int(fields["retransmittedPackets"]) > 0
    assert fields["permanentLosses"] == fields["rdmaFailedWqes"] == "0"
    assert fields["rdmaSourceD2hBytes"] == fields["rdmaDestinationH2dBytes"] == str(8192 * 32)


@pytest.mark.parametrize("config,path", [("rdma-host-p2p.cfg","host"),
    ("rdma-staged-p2p.cfg","staged"),("rdma-gpudirect-p2p.cfg","gpudirect")])
def test_user_presets_run(config, path):
    fields = run(f"--protocolConfig=configs/protocol_configs/{config}",
                 "--numNodes=2", "--gpusPerNode=2", "--dataSize=9000")
    assert fields["rdmaInitiator"] == "cpu" and fields["rdmaDataPath"] == path
    assert fields["rdmaCompletedRequests"] == "1"


@pytest.mark.parametrize("flag,message", [
    ("--rdmaInitiator=bad","rdmaInitiator"),
    ("--rdmaDataPath=bad","rdmaDataPath"),
    ("--rdmaCpuWorkers=0","workers"),
    ("--rdmaStagingBufferBytes=1","staging"),
])
def test_invalid_independent_mode_settings(flag,message):
    output = run(*BASE,flag,success=False)
    assert message.lower() in output.lower()
