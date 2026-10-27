"""Execution, persistence, validation, and process-lifecycle tests for the web API."""

import hashlib
import io
import json
import sys
import time
import zipfile
from pathlib import Path

import pytest
from fastapi.testclient import TestClient

from webui.app import create_app
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
