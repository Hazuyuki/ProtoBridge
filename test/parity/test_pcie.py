"""Packet-level PCIe exercised through native RDMA, not a standalone timing formula."""
import csv

import pytest

from test_ibgda import run, pytestmark  # noqa: F401

BASE = ["--numNodes=2", "--gpusPerNode=2", "--interNodeTopology=host",
        "--interNodeFabricType=RoCE", "--interNodeLatency=0", "--collective=rdma",
        "--algorithm=p2p", "--rdmaSubmission=detailed", "--pcieModel=tlp"]


@pytest.mark.parametrize("actor", ["cpu", "gpu"])
@pytest.mark.parametrize("path", ["host", "gpudirect", "staged"])
@pytest.mark.parametrize("opcode", ["write", "read", "send", "write_imm", "send_imm",
                                    "fetch_add", "compare_swap", "put_signal", "signal"])
def test_native_operations_use_pcie_and_preserve_semantics(actor, path, opcode):
    f = run(*BASE, f"--rdmaInitiator={actor}", f"--rdmaDataPath={path}",
            f"--rdmaOperation={opcode}", "--dataSize=3017", "--p2pIterations=3",
            "--rdmaStagingChunkBytes=1024", "--rdmaStagingBufferBytes=2048")
    assert f["pcieModel"] == "tlp"
    assert f["rdmaOperationsCompleted"] == f["rdmaCompletedRequests"] == "3"
    assert f["rdmaFailedRequests"] == f["rdmaFailedWqes"] == "0"
    assert int(f["pcieReadRequests"]) > 0 and int(f["pcieCompletionTlps"]) > 0
    assert int(f["pcieBytes_wqe"]) == int(f["rdmaSubmittedWqes"]) * 64
    assert int(f["pcieBytes_doorbell"]) == int(f["rdmaDoorbells"]) * 8
    assert int(f["pcieBytes_cq"]) >= int(f["rdmaCqes"]) * 64
    if opcode == "fetch_add":
        assert f["rdmaRemoteValue"] == f["rdmaAtomicExecutions"] == "3"
    if opcode in {"put_signal", "signal"}:
        assert f["rdmaSignalUpdates"] == "3" and f["rdmaSignalValue"] == "1"


def test_small_credits_and_queues_backpressure_instead_of_losing_data():
    f = run(*BASE, "--rdmaOperation=read", "--dataSize=32771", "--p2pIterations=4",
            "--pcieTags=2", "--pcieQueuePackets=1", "--pciePostedHeaderCredits=1",
            "--pciePostedDataCredits=16", "--pcieCompletionHeaderCredits=1",
            "--pcieCompletionDataCredits=16", "--pcieNonPostedHeaderCredits=1",
            "--pcieReplayBufferBytes=280", "--pcieCreditUpdateNs=300")
    assert int(f["pcieCreditStalls"]) > 0
    assert int(f["pciePeakQueuePackets"]) <= 1
    assert int(f["pciePeakOutstandingReads"]) <= 2
    assert f["rdmaReadResponseBytes"] == str(32771 * 4)


def test_pcie_trace_has_real_packets_control_traffic_and_credit_balances(tmp_path):
    path = tmp_path / "pcie.csv"
    f = run(*BASE, "--rdmaOperation=read", "--dataSize=9000", f"--pcieTraceFile={path}")
    rows = list(csv.DictReader(path.open()))
    tx = [r for r in rows if r["event"] == "tx"]
    assert {r["type"] for r in tx} == {"MRd", "MWr", "CplD"}
    assert {"doorbell", "wqe", "payload", "cq"} <= {r["traffic"] for r in tx}
    assert {"credit_update", "link_ack", "read_complete", "write_complete"} <= {r["event"] for r in rows}
    assert len(tx) == int(f["pcieTlpHops"])
    assert all(int(r[credit]) >= 0 for r in rows for credit in ["ph", "pd", "nph", "npd", "cplh", "cpld"])


@pytest.mark.parametrize("path", ["gpudirect", "staged"])
def test_legacy_pcie_bandwidth_is_not_double_charged(path):
    args = [*BASE, f"--rdmaDataPath={path}", "--dataSize=8192"]
    a = run(*args)
    b = run(*args, "--rdmaPcieReadGbps=1", "--rdmaPcieWriteGbps=1")
    assert a["simTimeNs"] == b["simTimeNs"]


def test_trace_limit_does_not_change_execution_or_counters(tmp_path):
    args = [*BASE, "--dataSize=8192"]
    a = run(*args)
    path = tmp_path / "limited.csv"
    b = run(*args, f"--pcieTraceFile={path}", "--pcieTraceMaxEvents=17")
    assert b["pcieTraceEvents"] == "17" and b["pcieTraceTruncated"] == "1"
    assert len(list(csv.DictReader(path.open()))) == 17
    assert a["simTimeNs"] == b["simTimeNs"] and a["pcieWireBytes"] == b["pcieWireBytes"]


def test_root_complex_has_separate_forwarding_latency():
    args = [*BASE, "--dataSize=64", "--rdmaDataPath=gpudirect", "--rdmaInitiator=gpu"]
    switch = run(*args, "--pcieTopology=switch")
    root = run(*args, "--pcieTopology=root", "--pcieRootLatencyNs=1000")
    assert int(root["simTimeNs"]) > int(switch["simTimeNs"])


@pytest.mark.parametrize("config", ["ibgda-p2p.cfg", "rdma-staged-p2p.cfg", "rdma-gpudirect-p2p.cfg"])
def test_existing_config_stencils_accept_tlp_model(config):
    f = run(f"--protocolConfig=configs/protocol_configs/{config}", "--numNodes=2",
            "--gpusPerNode=2", "--pcieModel=tlp", "--dataSize=9000")
    assert f["pcieModel"] == "tlp" and f["rdmaCompletedRequests"] == "1"


@pytest.mark.parametrize("path", ["gpudirect", "staged"])
@pytest.mark.parametrize("collective", ["allreduce", "allgather", "alltoall"])
def test_hierarchical_collectives_wait_for_pcie_and_memory_readiness(path, collective):
    f = run("--numNodes=2", "--gpusPerNode=2", "--interNodeTopology=host",
            "--interNodeFabricType=RoCE", "--interNodeLatency=0", f"--collective={collective}",
            "--algorithm=hierarchical", "--rdmaSubmission=detailed", "--pcieModel=tlp",
            f"--rdmaDataPath={path}", "--dataSize=4096", "--rdmaStagingChunkBytes=1024",
            "--rdmaStagingBufferBytes=2048")
    assert int(f["rdmaCompletedRequests"]) > 0
    assert f["rdmaSubmittedRequests"] == f["rdmaCompletedRequests"]
    assert int(f["simTimeNs"]) >= int(f["rdmaDataReadyNs"])


@pytest.mark.parametrize("pattern", ["incast", "fanout"])
def test_shared_host_paths_with_concurrent_bulk_flows(pattern):
    f = run("--numNodes=2", "--gpusPerNode=2", "--interNodeTopology=host",
            "--interNodeFabricType=RoCE", "--interNodeLatency=0", f"--collective={pattern}",
            "--rdmaSubmission=detailed", "--rdmaInitiator=cpu", "--rdmaDataPath=staged",
            "--pcieModel=tlp", "--dataSize=4096", "--pcieQueuePackets=2",
            "--rdmaStagingChunkBytes=1024", "--rdmaStagingBufferBytes=2048")
    assert f["rdmaSubmittedRequests"] == f["rdmaCompletedRequests"]
    assert f["rdmaSourceD2hBytes"] == f["rdmaDestinationH2dBytes"]


def test_registered_address_alignment_reaches_pcie_splitter(tmp_path):
    path = tmp_path / "unaligned.csv"
    run(*BASE, "--dataSize=23", "--rdmaLocalAddress=4093", "--rdmaRemoteAddress=8191",
        f"--pcieTraceFile={path}")
    tx = [r for r in csv.DictReader(path.open()) if r["event"] == "tx" and r["traffic"] == "payload"]
    assert any(r["type"] == "MRd" and int(r["address"]) % 4096 == 4093 and r["bytes"] == "3" for r in tx)
    assert any(r["type"] == "MWr" and int(r["address"]) % 4096 == 4095 and r["bytes"] == "1" for r in tx)


def test_timing_only_bulk_keeps_fragment_offsets(tmp_path):
    path = tmp_path / "bulk.csv"
    run("--numNodes=2", "--gpusPerNode=2", "--interNodeTopology=host",
        "--interNodeFabricType=RoCE", "--collective=p2p", "--rootRank=1",
        "--rdmaSubmission=detailed", "--pcieModel=tlp", "--dataSize=9000",
        f"--pcieTraceFile={path}")
    tx = [r for r in csv.DictReader(path.open()) if r["event"] == "tx" and r["traffic"] == "payload"]
    # The 9000-B bulk is split into multiple non-MRRS-aligned network fragments.
    # Restarting every DMA at zero would leave every MRd start aligned to 512 B.
    assert any(r["type"] == "MRd" and int(r["address"]) % 512 != 0 for r in tx)


def test_atomic_returns_the_nic_buffered_old_value_without_another_pcie_read():
    f = run(*BASE, "--rdmaOperation=fetch_add", "--dataSize=8")
    assert f["rdmaRemoteValue"] == "1"
    # Responder reads and updates 8 B; requester stores the 8-B returned value.
    assert f["pcieBytes_payload"] == "24"


@pytest.mark.parametrize("flags,message", [
    (["--pcieGeneration=6"], "Gen3--5"),
    (["--pcieTags=0"], "pcieTags"),
    (["--pcieMps=300"], "pcieMps"),
    (["--pciePostedDataCredits=1"], "credits"),
    (["--pcieReplayBufferBytes=1"], "ReplayBuffer"),
    (["--rdmaSubmission=startup"], "requires"),
])
def test_invalid_pcie_settings_are_rejected(flags, message):
    assert message.lower() in run(*BASE, *flags, success=False).lower()
