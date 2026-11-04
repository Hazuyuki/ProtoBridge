"""Real-binary checks of settings whose native source/precedence can change."""

import time
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
