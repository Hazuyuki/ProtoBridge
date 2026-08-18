"""Pytest config: make the surrogate theory package importable as a top-level
module (``import whitebox_surrogate_v2``), so its directory must be on
sys.path when the tests run.
"""
import os
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
_d = os.path.join(_HERE, "theory")
if os.path.isdir(_d) and _d not in sys.path:
    sys.path.insert(0, _d)
