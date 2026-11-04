"""Translation boundaries and compatibility with saved runs."""

import re
from html.parser import HTMLParser

from webui.catalog import Catalog
from webui.i18n import CATALOG, canonical_message, catalog_labels, run_labels

from webui.tests.test_webui import REPO, client, executable, submit, wait  # noqa: F401


def test_catalog_has_both_languages_and_preserves_placeholders():
    assert CATALOG["en"].keys() == CATALOG["zh"].keys()
    for key, english in CATALOG["en"].items():
        assert english.strip() and CATALOG["zh"][key].strip()
        assert set(re.findall(r"\{\w+\}", english)) == set(re.findall(r"\{\w+\}", CATALOG["zh"][key])), key
    for legacy, key in CATALOG["aliases"].items():
        assert key in CATALOG["en"]
        assert set(re.findall(r"\{\w+\}", legacy)) == set(re.findall(r"\{\w+\}", key))


def test_static_bindings_have_translations():
    class Bindings(HTMLParser):
        def handle_starttag(self, tag, attrs):
            for attribute, key in attrs:
                if attribute == "data-i18n" or attribute.startswith("data-i18n-"):
                    assert key in CATALOG["en"], key

    Bindings().feed((REPO / "webui/static/index.html").read_text())


def test_catalog_metadata_is_english_and_source_text_is_untouched(client):
    original = Catalog(REPO).listing()
    metadata = client.get("/api/catalog").json()
    assert metadata == catalog_labels(original)
    for actual, source in zip(metadata["configs"], original["configs"]):
        assert {key: actual[key] for key in ["text", "profile_text", "defaults", "op"]} == {
            key: source[key] for key in ["text", "profile_text", "defaults", "op"]
        }
        assert not re.search(r"[\u3400-\u9fff]", actual["title"] + actual["description"])
    assert metadata["profiles"] == original["profiles"]
    assert metadata["options"]["algorithms"]["hierarchical"] == "Hierarchical"


def test_canonical_messages_preserve_native_parameters_and_unknown_text():
    assert canonical_message("第 9 行重复定义 bandwidthGbps。") == "Line 9 defines bandwidthGbps more than once."
    assert canonical_message("运行超过 120 秒，已停止。") == "The run exceeded 120 seconds and was stopped."
    assert canonical_message("vcCredits 范围为 1–1000。") == "vcCredits must be between 1 and 1000."
    assert canonical_message("native parser output: Ω / 实验A") == "native parser output: Ω / 实验A"


def test_validation_and_http_errors_default_to_english(client):
    response = client.post("/api/preview", json={"num_nodes": 32, "gpus_per_node": 64})
    assert response.status_code == 422
    assert response.json()["detail"][0]["msg"] == "Multi-node scenarios need 2–256 ranks in total."
    response = client.post("/api/preview", json={"cli_overrides": {"unsupported": 1}})
    assert response.status_code == 422
    assert response.json()["detail"] == "Unsupported CLI flag unsupported. Edit hardware and protocol parameters in the configuration."
    assert client.get("/api/runs/missing").json()["detail"] == "Run not found."
    assert client.post("/api/runs", json={}, headers={"Origin": "https://different.example"}).json()["detail"] == "Submit jobs from the current WebUI origin."


def test_default_and_explicit_names_and_native_artifacts(client):
    identifier = submit(client, config_id="h200-request-response.cfg")
    run = wait(client, identifier)
    assert run["name_is_default"] and run["name"] == "Request / response"
    identifier = submit(client, config_id="h200-request-response.cfg", name="请求 / 响应")
    explicit = wait(client, identifier)
    assert explicit["name"] == "请求 / 响应" and not explicit["name_is_default"]
    assert client.get(f"/api/runs/{identifier}/artifacts/config.cfg").text == explicit["config"]
    assert "RESULT_START" in client.get(f"/api/runs/{identifier}/log").json()["text"]
    summaries = client.get("/api/runs").json()["runs"]
    assert summaries[0]["name"] == "请求 / 响应" and not summaries[0]["name_is_default"]
    assert summaries[1]["name"] == "Request / response" and summaries[1]["name_is_default"]


def test_legacy_run_translation_does_not_modify_saved_or_user_data():
    legacy = {"name": "请求 / 响应", "error": "用户取消了这次运行。", "request": {"name": ""},
              "config": "# 保留原始内容", "notices": ["拓扑维度决定实际 GPU 数为 8。"]}
    translated = run_labels(legacy)
    assert translated["name"] == "Request / response"
    assert translated["error"] == "The user cancelled this run."
    assert translated["notices"] == ["The topology dimensions set the actual GPU count to 8."]
    assert translated["config"] == legacy["config"]
    assert legacy["error"] == "用户取消了这次运行。"
    legacy["request"]["name"] = "请求 / 响应"
    assert run_labels(legacy)["name"] == "请求 / 响应"
