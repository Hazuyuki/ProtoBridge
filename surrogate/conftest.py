"""Pytest config: make the surrogate sub-packages importable as top-level modules.

The surrogate modules resolve sibling imports via their own directory on
sys.path (e.g. ``dse_topo_surrogate`` does ``from topo_grammar import ...``),
so each sub-package dir must be on sys.path when the tests run.
"""
import os
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
for _sub in ("theory", "analytical", "topo"):
    _d = os.path.join(_HERE, _sub)
    if os.path.isdir(_d) and _d not in sys.path:
        sys.path.insert(0, _d)
