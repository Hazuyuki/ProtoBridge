"""Canonical English presentation messages, including saved Chinese run records."""

import json
import re
from functools import lru_cache
from pathlib import Path

CATALOG = json.loads((Path(__file__).parent / "static/locales.json").read_text())


def _templates():
    sources = {**{key: key for key in CATALOG["en"]},
               **{value: key for key, value in CATALOG["zh"].items()},
               **CATALOG["aliases"]}
    patterns = []
    for source, key in sources.items():
        names = re.findall(r"\{(\w+)\}", source)
        if not names:
            continue
        parts = re.split(r"\{\w+\}", source)
        expression = r"([\s\S]+?)".join(re.escape(part) for part in parts)
        patterns.append((len(source) - len(names) * 8, re.compile(expression), key, names))
    return sorted(patterns, key=lambda item: item[0], reverse=True)


PATTERNS = _templates()


@lru_cache(maxsize=1024)
def canonical_message(message: str) -> str:
    """Translate only application-owned text; unknown/native text passes through."""
    key = CATALOG["aliases"].get(message, message)
    if key in CATALOG["en"]:
        return key
    for _, pattern, target, names in PATTERNS:
        match = pattern.fullmatch(message)
        if match:
            params = dict(zip(names, match.groups()))
            return re.sub(r"\{(\w+)\}", lambda field: params[field[1]], target)
    return message


def catalog_labels(value):
    """Copy catalog metadata without translating source text, values or defaults."""
    if isinstance(value, list):
        return [catalog_labels(item) for item in value]
    if isinstance(value, dict):
        return {key: {name: canonical_message(label) for name, label in item.items()}
                if key == "algorithms" and isinstance(item, dict) else
                canonical_message(item) if key in {
                    "label", "title", "subtitle", "description", "hint"
                } and isinstance(item, str) else catalog_labels(item)
                for key, item in value.items()}
    return value


def run_labels(run: dict, original: dict | None = None) -> dict:
    original = original or run
    default_name = not original.get("request", {}).get("name", "").strip()
    return {**run, "name_is_default": default_name,
            "name": canonical_message(run["name"]) if default_name else run["name"],
            "error": canonical_message(run["error"]) if run.get("error") else run.get("error"),
            **({"notices": [canonical_message(message) for message in run["notices"]]}
               if "notices" in run else {})}
