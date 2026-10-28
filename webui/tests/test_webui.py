"""Execution, persistence, validation, and process-lifecycle tests for the web API."""

import hashlib
import io
import json
import re
import shutil
import sys
import time
import zipfile
from pathlib import Path

import pytest
from fastapi.testclient import TestClient

from webui.app import create_app
import webui.app as web_app
from webui.catalog import CLI_FIELDS
from webui.simulation import parse_result

REPO = Path(__file__).resolve().parents[2]
TERMINAL = {"completed", "incomplete", "failed", "cancelled", "timed_out", "interrupted"}


@pytest.fixture
def executable(tmp_path):
    path = tmp_path/"fake-simulator"
    path.write_text(f"#!{sys.executable}\n" + r'''
import csv, sys, time
args = dict(value[2:].split("=", 1) for value in sys.argv[1:])
size = int(args["dataSize"])
if size in {42, 43}:
    print("starting long operation", flush=True)
    time.sleep(30)
if size == 14:
    print("invalid native configuration", flush=True)
    sys.exit(2)
if size == 15:
    print("no result block")
    sys.exit(0)
print("RESULT_START")
print("status=" + ("incomplete" if size == 13 else "complete"))
print("simTimeNs=38308")
print("simTimeUs=38.3")
print("endpointTxPackets=22")
print("RESULT_END")
with open(args["stepTraceFile"], "w") as stream:
    writer = csv.writer(stream)
    writer.writerow(["gpu", "step", "collectiveStartNs", "completeNs", "sinceStartNs"])
    writer.writerow([0, 0, 100, 1100, 1000])
    writer.writerow([0, 1, 100, 2100, 2000])
''')
    path.chmod(0o755)
    return path


@pytest.fixture
def client(tmp_path, executable):
    with TestClient(create_app(REPO, tmp_path/"runs", executable)) as app:
        yield app


def wait(client, identifier, status=None, timeout=5):
    end = time.monotonic()+timeout
    while time.monotonic() < end:
        job = client.get("/api/runs/"+identifier).json()
        if job["status"] == status if status else job["status"] in TERMINAL:
            return job
        time.sleep(0.025)
    raise AssertionError(f"Task did not reach {status or 'a terminal status'}: {job}")


def submit(client, **values):
    response = client.post("/api/runs", json=values)
    assert response.status_code == 202, response.text
    return response.json()["id"]


def test_catalog_and_real_parameter_preview(client):
    catalog = client.get("/api/catalog").json()
    assert len(catalog["configs"]) == 11
    assert client.get("/").status_code == 200
    assert client.get("/static/app.js").status_code == 200
    body = {"stack_overrides": {"bandwidthGbps": 85}, "num_nodes": 2, "gpus_per_node": 4}
    preview = client.post("/api/preview", json=body)
    assert preview.status_code == 200
    assert preview.json()["resolved"]["bandwidthGbps"] == "85"
    assert preview.json()["summary"]["ranks"] == 8


def test_form_catalog_only_lists_native_algorithms_and_topologies(client):
    options = client.get("/api/catalog").json()["options"]
    assert len(options["topologies"]) == 14
    assert "ndfullmesh" not in {item["value"] for item in options["topologies"]}
    assert "2dfullmeshclos" not in {item["value"] for item in options["topologies"]}
    allreduce = next(item for item in options["collectives"] if item["value"] == "allreduce")
    assert {"tree", "ncclring", "nvls", "hierarchical"} <= set(allreduce["algorithms"])


def test_page_loads_versioned_scripts_with_revalidation(client):
    response = client.get("/")
    assert response.headers["cache-control"] == "no-cache"
    assets = re.findall(r'(?:src|href)="(/static/[^" ]+\?v=[0-9a-f]{12})"', response.text)
    assert len(assets) == 3
    for asset in assets:
        loaded = client.get(asset)
        assert loaded.status_code == 200
        assert loaded.headers["cache-control"] == "no-cache"
    assert "正在加载可选操作" in response.text


def test_asset_version_changes_when_script_content_changes(tmp_path, executable, monkeypatch):
    static = tmp_path/"static"
    shutil.copytree(REPO/"webui/static", static)
    monkeypatch.setattr(web_app, "STATIC", static)
    with TestClient(create_app(REPO, tmp_path/"runs", executable)) as client:
        before = client.get("/").text
        old_url = re.search(r'src="(/static/app.js\?v=[0-9a-f]{12})"', before).group(1)
        with (static/"app.js").open("a") as stream:
            stream.write("\n// browser cache regression\n")
        after = client.get("/").text
        new_url = re.search(r'src="(/static/app.js\?v=[0-9a-f]{12})"', after).group(1)
        assert new_url != old_url
        assert client.get(new_url).text.endswith("// browser cache regression\n")


def test_operation_overrides_are_written_to_the_executed_snapshot(client):
    identifier = submit(client, op_overrides={"collective": "allgather", "algorithm": "fullmesh", "topology": "fullmesh"})
    run = wait(client, identifier)
    assert run["summary"]["collective"] == "allgather"
    assert "algorithm = fullmesh" in run["config"]
    assert "topology = fullmesh" in run["config"]
    assert not any(flag.startswith("--algorithm=") for flag in run["command"])


def test_topology_dimensions_determine_the_actual_rank_count(client):
    values = {"op_overrides": {"topology": "mesh"}, "cli_overrides": {"meshRows": 3, "meshCols": 4}}
    identifier = submit(client, **values)
    run = wait(client, identifier)
    assert run["summary"]["ranks"] == 12
    assert "--numGpus=12" in run["command"]
    assert "--meshRows=3" in run["command"]


def test_resolved_controls_follow_profile_precedence_and_stencil_credit_scope(client):
    response = client.post("/api/preview", json={"cli_overrides": {"llrEnabled": True, "fecN": 600, "vcCredits": 17}})
    settings = response.json()["summary"]["settings"]
    assert response.status_code == 200
    assert settings["llrEnabled"] == "0"  # profile wins
    assert settings["fecN"] == "544"
    assert settings["effectiveVcCredits"] == "17"  # inline VC stays CLI-sourced
    response = client.post("/api/preview", json={"config_id": "p2p-single.cfg", "stack_overrides": {"vcCredits": 23, "llrEnabled": True, "llrMode": "sack", "fecEnabled": False}})
    settings = response.json()["summary"]["settings"]
    assert settings["llrEnabled"] == "1" and settings["llrMode"] == "sack"
    assert settings["effectiveVcCredits"] == "23" and settings["fecEnabled"] == "0"


def test_fabric_forced_llr_is_visible_in_preview(client):
    response = client.post("/api/preview", json={"config_id": "eth-hierarchical.cfg", "num_nodes": 2, "gpus_per_node": 4, "stack_overrides": {"llrEnabled": False}})
    assert response.status_code == 200
    settings = response.json()["summary"]["settings"]
    assert settings["llrRequested"] == "0" and settings["llrEnabled"] == "1"
    assert any("强制开启" in notice for notice in response.json()["notices"])


def test_cli_only_controls_in_stack_do_not_claim_to_be_enabled(client):
    response = client.post("/api/preview", json={"stack_overrides": {"dcqcn": True, "packetLossRate": .1}, "cli_overrides": {"packetLossRate": .01}})
    assert response.status_code == 200
    settings = response.json()["summary"]["settings"]
    assert settings["dcqcn"] == "0" and settings["packetLossRate"] == "0.01"
    assert any("不会传给原生 CLI" in notice for notice in response.json()["notices"])


def test_minimal_profile_has_native_bundle_and_inline_defaults(client):
    minimal = "protocolModel = ns3::NcclProtocolModel\n"
    response = client.post("/api/preview", json={"profile_text": minimal})
    assert response.status_code == 200
    settings = response.json()["summary"]["settings"]
    assert settings["fecEnabled"] == "0" and settings["fecN"] == "0"
    assert settings["berInterRackOptical"] == "1e-9"
    response = client.post("/api/preview", json={"config_id": "p2p-single.cfg", "profile_text": minimal, "cli_overrides": {"vcCredits": 17, "llrEnabled": True}})
    settings = response.json()["summary"]["settings"]
    assert settings["fecEnabled"] == "1" and settings["fecN"] == "544"
    assert settings["vcCredits"] == "64" and settings["llrEnabled"] == "0"


@pytest.mark.parametrize("values", [
    {"op_overrides": {"profile": "/etc/passwd"}},
    {"op_overrides": {"algorithm": "unknown"}},
    {"op_overrides": {"algorithm": "nvls"}},
    {"op_overrides": {"collective": "broadcast", "algorithm": "tree"}},
    {"op_overrides": {"algorithm": "hierarchical"}},
    {"op_overrides": {"topology": "ndfullmesh"}},
    {"op_overrides": {"topology": "2dfullmeshclos"}},
    {"op_overrides": {"topology": "leafspine"}},
    {"op_overrides": {"topology": "mesh"}, "cli_overrides": {"meshRows": 32, "meshCols": 32}},
    {"stack_overrides": {"fecN": 514}},
    {"stack_overrides": {"fecT": 16}},
    {"stack_overrides": {"flowControl": "rate"}},
    {"stack_overrides": {"berIntraNodeElectrical": -0.1}},
    {"stack_overrides": {"vcCount": 256}},
    {"config_id": "p2p-single.cfg", "op_overrides": {"collective": "allreduce"}},
    {"cli_overrides": {"errorMode": "burst", "ber": 0.000001}},
    {"cli_overrides": {"errorMode": "burst"}, "stack_overrides": {"fecEnabled": False}},
    {"num_nodes": 2, "stack_overrides": {"interNodeFabricType": "ETH"}, "cli_overrides": {"dcqcn": True}},
    {"num_nodes": 2, "stack_overrides": {"interNodeFabricType": "ETH"}, "cli_overrides": {"switchPfcMode": 1}},
    {"num_nodes": 2, "stack_overrides": {"interNodeFabricType": "RoCE"}, "cli_overrides": {"voqDropThreshold": 4, "ecnThreshold": 4, "dcqcn": True, "interNodeTopology": "tor"}},
    {"num_nodes": 2, "stack_overrides": {"interNodeFabricType": "RoCE"}, "cli_overrides": {"switchPfcMode": 3}},
    {"op_overrides": {"topology": "leafspine"}, "cli_overrides": {"leafSwitches": 2, "spineSwitches": 2, "interSwitchMedium": "optical", "failedOpticalLink": 4}},
])
def test_incompatible_native_options_are_rejected_before_queuing(client, values):
    response = client.post("/api/runs", json=values)
    assert response.status_code == 422, response.text
    assert client.get("/api/runs").json()["runs"] == []


@pytest.mark.parametrize("values", [
    {"num_gpus": 1}, {"num_gpus": 8.5}, {"data_size": 0},
    {"num_nodes": 32, "gpus_per_node": 64},
    {"config_id": "../../README.md"},
    {"cli_overrides": {"stepTraceFile": "/tmp/owned"}},
    {"cli_overrides": {"ber": "$(touch /tmp/owned)"}},
    {"cli_overrides": {"packetLossRate": 1.1}},
    {"cli_overrides": {"rootRank": 8}},
    {"cli_overrides": {"randTraffic": 1}},
    {"stack_overrides": {"bandwidthGbps": 0}},
    {"stack_overrides": {"latencyNs": "1\n[op]"}},
    {"stack_overrides": {"interNodeFabricType": "unsupported"}},
    {"stack_overrides": {"interNodeTopology": "unsupported"}},
    {"profile_text": ""}, {"config_text": "[op]\ncollective = allreduce"},
])
def test_invalid_inputs_are_rejected_before_execution(client, values):
    response = client.post("/api/runs", json=values)
    assert response.status_code == 422, response.text
    assert client.get("/api/runs").json()["runs"] == []


def test_profile_path_cannot_read_server_files(client):
    config = "[stack]\nprofile = /etc/passwd\n[op]\ncollective = allreduce\nalgorithm = ring\n"
    response = client.post("/api/preview", json={"config_text": config})
    assert response.status_code == 422
    assert "profile" in response.json()["detail"]


def test_run_snapshots_inputs_trace_and_downloads(client, executable):
    source = REPO/"configs/protocol_configs/h200-ring-allreduce.cfg"
    before = source.read_bytes()
    identifier = submit(client, name="测试 <script>", stack_overrides={"bandwidthGbps": 85})
    run = wait(client, identifier)
    assert run["status"] == "completed"
    assert run["result"]["simTimeNs"] == 38308
    assert run["binary"]["sha256"] == hashlib.sha256(executable.read_bytes()).hexdigest()
    assert "bandwidthGbps = 85" in run["config"]
    assert "--numGpus=8" in run["command"]
    assert source.read_bytes() == before
    trace = client.get(f"/api/runs/{identifier}/trace").json()
    assert [row["sinceStartNs"] for row in trace["rows"]] == [1000, 2000]
    archive = client.get(f"/api/runs/{identifier}/artifacts/run.zip")
    assert archive.status_code == 200
    with zipfile.ZipFile(io.BytesIO(archive.content)) as z:
        assert {"config.cfg", "profile.profile", "stdout.log", "steps.csv", "result.json"} == set(z.namelist())
        assert json.loads(z.read("result.json"))["id"] == identifier
    log = client.get(f"/api/runs/{identifier}/log").json()
    assert "RESULT_START" in log["text"] and log["cursor"] > 0
    assert client.get(f"/api/runs/{identifier}/artifacts/README.md").status_code == 404


@pytest.mark.parametrize("size,status", [(13, "incomplete"), (14, "failed"), (15, "failed")])
def test_unsuccessful_results_are_not_reported_as_completed(client, size, status):
    identifier = submit(client, data_size=size)
    run = wait(client, identifier)
    assert run["status"] == status
    assert run["error"]


def test_timeout_stops_process_and_queue_continues(client):
    first = submit(client, data_size=42, timeout=1)
    second = submit(client)
    assert wait(client, first)["status"] == "timed_out"
    assert wait(client, second)["status"] == "completed"


def test_running_and_queued_jobs_can_be_cancelled(client):
    first = submit(client, data_size=43)
    wait(client, first, "running")
    second = submit(client)
    assert client.post(f"/api/runs/{second}/cancel").json()["status"] == "cancelled"
    client.post(f"/api/runs/{first}/cancel")
    assert wait(client, first)["status"] == "cancelled"
    assert client.get(f"/api/runs/{second}").json()["started_at"] is None


def test_runs_survive_restart_and_csv_escapes_formulas(tmp_path, executable):
    directory = tmp_path/"persistent"
    with TestClient(create_app(REPO, directory, executable)) as client:
        identifier = submit(client, name="=1+2")
        wait(client, identifier)
    with TestClient(create_app(REPO, directory, executable)) as client:
        assert client.get(f"/api/runs/{identifier}").json()["status"] == "completed"
        csv = client.get("/api/runs/export.csv")
        assert csv.status_code == 200 and "'=1+2" in csv.text


def test_missing_binary_is_actionable(tmp_path):
    with TestClient(create_app(REPO, tmp_path/"runs", tmp_path/"missing")) as client:
        assert client.get("/api/status").json()["ready"] is False
        response = client.post("/api/runs", json={})
        assert response.status_code == 503 and "./ns3 build" in response.json()["detail"]


def test_cross_origin_submission_rejected(client):
    response = client.post("/api/runs", json={}, headers={"Origin": "https://unrelated.example"})
    assert response.status_code == 403


def test_shutdown_interrupts_active_process_and_persists_status(tmp_path, executable):
    directory = tmp_path/"restart"
    with TestClient(create_app(REPO, directory, executable)) as client:
        identifier = submit(client, data_size=43)
        wait(client, identifier, "running")
    with TestClient(create_app(REPO, directory, executable)) as client:
        run = client.get(f"/api/runs/{identifier}").json()
        assert run["status"] == "interrupted" and run["finished_at"]


def test_exposed_cli_fields_exist_in_native_entry_point():
    source = (REPO/"scratch/gpu-cluster-sim.cc").read_text()
    import re

    actual_flags = set(re.findall(r'cmd\.AddValue\("(\w+)"', source))
    assert set(CLI_FIELDS) <= actual_flags


@pytest.mark.parametrize("output", [
    "status=complete\nsimTimeNs=12\n", "RESULT_START\nstatus=complete\nRESULT_END\n",
    "RESULT_START\nstatus=complete\nsimTimeNs=1\nsimTimeNs=2\nRESULT_END\n",
    "RESULT_START\nstatus=complete\nsimTimeNs=1\nRESULT_END\n"*2,
])
def test_ambiguous_or_malformed_result_blocks_rejected(output):
    with pytest.raises(ValueError):
        parse_result(output)
