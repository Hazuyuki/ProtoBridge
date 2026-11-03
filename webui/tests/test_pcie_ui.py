"""PCIe form validation and actual native runs with downloadable TLP traces."""
import csv
import io
import zipfile

import pytest
from fastapi.testclient import TestClient

from webui.app import create_app
from webui.tests.test_native_options import REPO, run, pytestmark  # noqa: F401


def test_pcie_controls_are_available_in_both_languages(tmp_path):
    with TestClient(create_app(REPO, tmp_path / "runs")) as client:
        catalog = client.get("/api/catalog").json()
    group = next(g for g in catalog["options"]["groups"] if g["id"] == "pcie")
    assert group["title"] == "Local PCIe"
    model = next(f for f in group["fields"] if f["key"] == "pcieModel")
    assert model["default"] == "bandwidth"
    assert {c["value"] for c in model["choices"]} == {"bandwidth", "tlp"}
    from webui.i18n import CATALOG
    assert CATALOG["zh"]["Local PCIe"] == "本机 PCIe"
    assert all(f["label"] in CATALOG["zh"] for f in group["fields"])


@pytest.mark.parametrize("path", ["host", "gpudirect", "staged"])
def test_tlp_form_reaches_native_runtime_and_downloads_trace(tmp_path, path):
    with TestClient(create_app(REPO, tmp_path / "runs")) as client:
        job = run(client, config_id="rdma-read.cfg", num_nodes=2, gpus_per_node=2,
                  data_size=3017, stack_overrides={"pcieModel": "tlp", "pcieTags": 2,
                    "rdmaDataPath": path, "rdmaStagingChunkBytes": 1024,
                    "rdmaStagingBufferBytes": 2048})
        result = job["result"]
        assert result["pcieModel"] == "tlp" and result["rdmaOperationsCompleted"] == 1
        assert result["pcieReadRequests"] > 0
        assert result["pciePeakOutstandingReads"] <= 2
        response = client.get(f"/api/runs/{job['id']}/artifacts/pcie.csv")
        assert response.status_code == 200
        rows = list(csv.DictReader(io.StringIO(response.text)))
        assert any(r["event"] == "tx" and r["type"] == "CplD" for r in rows)
        bundle = client.get(f"/api/runs/{job['id']}/artifacts/run.zip")
        with zipfile.ZipFile(io.BytesIO(bundle.content)) as archive:
            assert archive.read("pcie.csv").decode() == response.text


@pytest.mark.parametrize("overrides,message", [
    ({"rdmaSubmission": "startup", "pcieModel": "tlp"}, "require"),
    ({"pcieModel": "tlp", "pciePostedDataCredits": 1}, "credits"),
    ({"pcieModel": "tlp", "pcieReplayBufferBytes": 152}, "replay buffer"),
])
def test_pcie_invalid_resource_settings_are_rejected_before_launch(tmp_path, overrides, message):
    with TestClient(create_app(REPO, tmp_path / "runs")) as client:
        response = client.post("/api/preview", json={"config_id": "rdma-read.cfg",
            "num_nodes": 2, "gpus_per_node": 2, "stack_overrides": overrides})
    assert response.status_code == 422
    assert message.lower() in response.text.lower()
