#!/usr/bin/env python3
"""Evaluate both surrogate tree models against the FIXED-ns-3 tree ground truth.

Inputs (all read-only):
  results_tree_rerun.csv        fixed-ns-3 tree runs (written by run_tree_validation.py)
  paper_seed1_tree.json         old-ns-3 (parent binary, June) values for the same cells
  parent paper fork surrogate   /share_data/zhuyu/ns-3.47/doc/paper/experiments/whitebox_surrogate_v2.py
  protobridge 9/18 surrogate    ../../surrogate/theory/whitebox_surrogate_v2.py

Outputs: tree_validation_report.txt (+ CSV) in this directory.
"""
import csv
import importlib.util
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
# Repo root = three levels up from this script (examples/gpu-cluster/<this dir>)
PB_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))

# --- load both surrogate forks ---
# The paper fork (parent repo, read-only) provides the OLD tree formula for
# contrast; its location comes from PB_PAPER_FORK (default: the sibling of
# PB_ROOT in this workspace). A standalone checkout without the paper fork
# should set PB_PAPER_FORK or drop the paper-model columns.
PAPER_FORK = os.environ.get(
    "PB_PAPER_FORK",
    os.path.join(os.path.dirname(PB_ROOT), "doc/paper/experiments"))
sys.path.insert(0, PAPER_FORK)
import whitebox_surrogate_v2 as paper_v2          # old tree formula (eta_sw)

spec = importlib.util.spec_from_file_location(
    "pb_v2", os.path.join(PB_ROOT, "surrogate/theory/whitebox_surrogate_v2.py"))
pb_v2 = importlib.util.module_from_spec(spec)
sys.modules["pb_v2"] = pb_v2
spec.loader.exec_module(pb_v2)                    # recalibrated tree (9/18)

old_tree = paper_v2.make_h200_tree_theory()
new_tree = pb_v2.make_h200_tree_theory()

new_rows = [r for r in csv.DictReader(open(os.path.join(HERE, "results_tree_rerun.csv")))
            if r["status"] == "complete" and r["sim_us"]]
paper = {}
for k, v in json.load(open(os.path.join(HERE, "paper_seed1_tree.json"))).items():
    if v is None:
        continue
    parts = k.strip("()").split(", ")
    paper[(int(parts[0]), int(parts[1]), float(parts[2]), int(parts[3]),
           int(parts[4]), int(parts[5]))] = v

def pred_old(r):
    return old_tree.predict(int(r["data_size"]), int(r["num_gpus"]), "tree", int(r["cr"]),
                            ber=float(r["ber"]), fec_enabled=int(r["fec"]) > 0,
                            llr_enabled=int(r["llr"]) > 0,
                            startup_us=15000 / 1000)

def pred_new(r):
    return new_tree.predict(int(r["data_size"]), int(r["num_gpus"]), "tree", int(r["cr"]),
                            ber=float(r["ber"]), fec_enabled=int(r["fec"]) > 0,
                            llr_enabled=int(r["llr"]) > 0,
                            startup_us=15000 / 1000, collective="allreduce")

def mape(errs):
    return sum(errs) / len(errs) if errs else float("nan")

rows_out = []
report = []
for r in new_rows:
    k = (int(r["num_gpus"]), int(r["data_size"]), float(r["ber"]),
         int(r["fec"]), int(r["llr"]), int(r["cr"]))
    sim_new = float(r["sim_us"])
    po, pn = pred_old(r), pred_new(r)
    old_paper = paper.get(k)
    rows_out.append(dict(num_gpus=k[0], data_size=k[1], ber=k[2], fec=k[3],
                         llr=k[4], cr=k[5], sim_fixed=sim_new, sim_old=old_paper,
                         pred_old=round(po, 2), pred_new=round(pn, 2)))

# overall + per-size MAPE against FIXED ns-3
err_old, err_new, err_papervs = [], [], []
by_size = {}
for r in rows_out:
    eo = abs(r["pred_old"] - r["sim_fixed"]) / r["sim_fixed"] * 100
    en = abs(r["pred_new"] - r["sim_fixed"]) / r["sim_fixed"] * 100
    err_old.append(eo); err_new.append(en)
    sz = r["data_size"]
    by_size.setdefault(sz, [[], []])
    by_size[sz][0].append(eo); by_size[sz][1].append(en)
    if r["sim_old"]:
        err_papervs.append(abs(r["sim_old"] - r["sim_fixed"]) / r["sim_fixed"] * 100)

report.append(f"Tree ground-truth revalidation: fixed ns-3 (protobridge de16a7c+) vs surrogate models")
report.append(f"runs: {len(rows_out)} complete (seed 1, stratified over the paper config space)")
report.append("")
report.append(f"vs FIXED ns-3:   paper fork (old eta_sw*log2N formula): MAPE {mape(err_old):.1f}%")
report.append(f"                 9/18 recal (alpha(N) trunk-load):       MAPE {mape(err_new):.1f}%")
report.append("")
report.append("per data size (old -> new MAPE vs fixed ns-3):")
for sz in sorted(by_size):
    eo, en = mape(by_size[sz][0]), mape(by_size[sz][1])
    report.append(f"  {sz/2**20:>9.3f} MiB  n={len(by_size[sz][0]):>2}   {eo:>6.1f}% -> {en:>6.1f}%")
report.append("")
report.append(f"sim drift (old parent ns-3 vs fixed protobridge ns-3, same cells): "
              f"mean |delta| {mape(err_papervs):.1f}% over {len(err_papervs)} matched cells")

with open(os.path.join(HERE, "tree_validation.csv"), "w", newline="") as f:
    w = csv.DictWriter(f, fieldnames=list(rows_out[0].keys()))
    w.writeheader(); w.writerows(rows_out)
with open(os.path.join(HERE, "tree_validation_report.txt"), "w") as f:
    f.write("\n".join(report) + "\n")
print("\n".join(report))
