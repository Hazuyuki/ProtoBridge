"""Real-binary checks of settings whose native source/precedence can change."""

import time
import csv
import io
import zipfile
from pathlib import Path

import pytest
from fastapi.testclient import TestClient

from webui.app import create_app
from webui.simulation import TERMINAL, find_binary

REPO = Path(__file__).resolve().parents[2]
pytestmark = pytest.mark.skipif(find_binary(REPO) is None, reason="Build gpu-cluster-sim to run native option checks.")
CLEAN = {"berIntraNodeElectrical": 0, "berIntraRackElectrical": 0, "berInterRackOptical": 0}


def run(client, **body):
    submitted = client.post("/api/runs", json={"timeout": 10, **body})
    assert submitted.status_code == 202, submitted.text
    identifier = submitted.json()["id"]
    end = time.monotonic() + 12
    while time.monotonic() < end:
        job = client.get(f"/api/runs/{identifier}").json()
        if job["status"] in TERMINAL:
            assert job["status"] == "completed", client.get(f"/api/runs/{identifier}/log").text
            return job
        time.sleep(0.02)
    raise AssertionError("Native simulation did not finish.")


@pytest.mark.parametrize("profile_text", [None, "protocolModel = ns3::NcclProtocolModel\n"])
def test_fec_disable_reaches_the_real_stencil_endpoint(tmp_path, profile_text):
    with TestClient(create_app(REPO, tmp_path/"runs")) as client:
        on = run(client, config_id="p2p-single.cfg", profile_text=profile_text, stack_overrides={**CLEAN, "fecEnabled": True})
        off = run(client, config_id="p2p-single.cfg", profile_text=profile_text, stack_overrides={**CLEAN, "fecEnabled": False})
    assert on["result"]["simTimeNs"] > off["result"]["simTimeNs"]
    assert off["summary"]["settings"]["fecEnabled"] == "0"
    assert "fecEnabled = 0" in off["config"]


def test_minimal_collective_profile_reports_fec_disabled_until_configured(tmp_path):
    with TestClient(create_app(REPO, tmp_path/"runs")) as client:
        bare = run(client, profile_text="protocolModel = ns3::NcclProtocolModel\n", stack_overrides=CLEAN)
        configured = run(client, profile_text="protocolModel = ns3::NcclProtocolModel\n",
                         op_overrides={"topology": "switched"},
                         stack_overrides={**CLEAN, "fecN": 544, "fecK": 514, "fecT": 15},
                         cli_overrides={"ber": .00001})
    assert bare["summary"]["settings"]["fecEnabled"] == "0"
    assert configured["summary"]["settings"]["fecEnabled"] == "1"
    assert bare["result"]["fecCorrectable"] == 0
    assert configured["result"]["fecCorrectable"] > 0


def test_collective_form_changes_the_real_native_injector(tmp_path):
    with TestClient(create_app(REPO, tmp_path/"runs")) as client:
        job = run(client, data_size=65536, op_overrides={"collective": "allgather", "algorithm": "fullmesh", "topology": "fullmesh"})
    assert job["result"]["collective"] == "allgather"
    assert job["result"]["algorithm"] == "fullmesh"
    assert job["result"]["topology"] == "fullmesh"
    assert job["result"]["numGpus"] == job["summary"]["ranks"] == 8


def test_optical_fault_uses_actual_link_inventory_and_routes(tmp_path):
    with TestClient(create_app(REPO, tmp_path/"runs")) as client:
        job = run(client, op_overrides={"topology": "leafspine"},
                  cli_overrides={"leafSwitches": 2, "spineSwitches": 2, "interSwitchMedium": "optical", "failedOpticalLink": 0})
    assert job["result"]["opticalInterSwitchLinks"] == 4
    assert job["result"]["operationalOpticalInterSwitchLinks"] == 3
    assert job["result"]["failureRoutesComplete"] == 1
    assert job["result"]["failureUnreachableGpuPairs"] == 0


def test_sack_recovers_actual_random_packet_loss(tmp_path):
    with TestClient(create_app(REPO, tmp_path/"runs")) as client:
        job = run(client, config_id="p2p-roce-lossy-sack.cfg", num_nodes=2, gpus_per_node=2,
                  stack_overrides={**CLEAN, "interNodeFabricType": "RoCE", "interNodeTopology": "host", "vcCredits": 100000000},
                  cli_overrides={"bulkChunkSize": 4096, "packetLossRate": .01, "llrTimeoutRetransmit": True,
                                 "llrRetryTimeout": 200000, "llrRetryLimit": 50, "soTxWindow": 64, "llrBufferSize": 10000})
    assert job["result"]["retransmittedPackets"] > 0
    assert job["result"]["permanentLosses"] == 0


def test_ibgda_form_reaches_native_runtime_and_waits_for_completion(tmp_path):
    with TestClient(create_app(REPO, tmp_path/"runs")) as client:
        job = run(client, config_id="ibgda-p2p.cfg", num_nodes=2, gpus_per_node=2,
                  data_size=9000, stack_overrides={"ibgdaQueueDepth": 16,
                  "ibgdaDoorbellBatch": 8, "ibgdaCqModeration": 16})
    assert job["result"]["rdmaSubmission"] == "ibgda"
    assert job["result"]["ibgdaSubmittedWqes"] == job["result"]["ibgdaCompletedWqes"] == 1
    assert job["result"]["ibgdaStartupSamples"] == 1
    assert job["result"]["simTimeNs"] >= job["result"]["dataDeliveryDurationNs"]


@pytest.mark.parametrize("config,actor,path", [
    ("rdma-host-p2p.cfg", "cpu", "host"),
    ("rdma-staged-p2p.cfg", "cpu", "staged"),
    ("rdma-gpudirect-p2p.cfg", "cpu", "gpudirect"),
    ("ibgda-p2p.cfg", "gpu", "gpudirect"),
])
def test_rdma_presets_run_with_downloadable_wr_trace(tmp_path, config, actor, path):
    with TestClient(create_app(REPO, tmp_path/"runs")) as client:
        job = run(client, config_id=config, num_nodes=2, gpus_per_node=2,
                  data_size=9000, stack_overrides={"rdmaStagingChunkBytes": 1024,
                                                 "rdmaStagingBufferBytes": 2048})
        result = job["result"]
        assert result["rdmaInitiator"] == actor and result["rdmaDataPath"] == path
        assert result["rdmaSubmittedRequests"] == result["rdmaCompletedRequests"] == 1
        expected = 9 if path == "staged" else 1
        assert result["rdmaSubmittedWqes"] == result["rdmaCompletedWqes"] == expected
        artifact = client.get(f"/api/runs/{job['id']}/artifacts/rdma.csv")
        assert artifact.status_code == 200
        rows = list(csv.DictReader(io.StringIO(artifact.text)))
        assert len(rows) == expected
        assert sum(int(row["bytes"]) for row in rows) == 9000
        archive = client.get(f"/api/runs/{job['id']}/artifacts/run.zip")
        with zipfile.ZipFile(io.BytesIO(archive.content)) as bundle:
            assert bundle.read("rdma.csv").decode() == artifact.text


def test_canonical_form_overrides_legacy_profile_and_stack_aliases(tmp_path):
    source = (REPO/"configs/protocol_configs/ibgda-p2p.cfg").read_text()
    source = source.replace("[stack]", "[stack]\nibgdaWqeBuildNs = 20000")
    with TestClient(create_app(REPO, tmp_path/"runs")) as client:
        job = run(client, config_id="ibgda-p2p.cfg", config_text=source,
                  num_nodes=2, gpus_per_node=2, data_size=64,
                  stack_overrides={"rdmaSubmission": "detailed", "rdmaGpuWqeBuildNs": 1234})
    assert job["summary"]["settings"]["rdmaGpuWqeBuildNs"] == "1234"
    assert "rdmaGpuWqeBuildNs = 1234" in job["config"]
    assert "ibgdaWqeBuildNs" not in job["config"]
    assert job["result"]["rdmaMeanStartupNs"] >= 1234


@pytest.mark.parametrize("name,opcode", [("rdma-read", "read"), ("rdma-send", "send"),
                                        ("rdma-atomic", "fetch_add"), ("rdma-put-signal", "put_signal")])
def test_rdma_operation_presets_execute_native_verbs(tmp_path, name, opcode):
    with TestClient(create_app(REPO, tmp_path/"runs")) as client:
        job = run(client, config_id=name+".cfg", num_nodes=2, gpus_per_node=2,
                  data_size=3017, stack_overrides={"rdmaDataPath": "staged",
                      "rdmaStagingChunkBytes": 1024, "rdmaStagingBufferBytes": 2048})
        artifact = client.get(f"/api/runs/{job['id']}/artifacts/rdma.csv")
    assert job["result"]["rdmaOperation"] == opcode
    assert job["result"]["rdmaOperationsCompleted"] == 1
    rows = list(csv.DictReader(io.StringIO(artifact.text)))
    assert all(row["opcode"] in {opcode, "signal"} for row in rows)


def test_web_form_can_select_immediate_and_delayed_receives(tmp_path):
    with TestClient(create_app(REPO, tmp_path/"runs")) as client:
        job = run(client, config_id="rdma-send.cfg", num_nodes=2, gpus_per_node=2,
                  data_size=731, stack_overrides={"rdmaOperation": "send_imm", "rdmaImmediate": 65537,
                      "rdmaReceivePostDelayNs": 5000, "rdmaRnrRetryLimit": 20})
    assert job["result"]["rdmaOperation"] == "send_imm"
    assert job["result"]["rdmaReceiveCompletions"] == 1
    assert job["result"]["rdmaRnrRetries"] > 0
