#!/usr/bin/env python3
"""Render the five ProtoBridge CSV sweeps as one PDF page and one 200-dpi PNG.

Run with Python and matplotlib (developed against 3.10.8; any >= 3.5 should
render identically). Input and output paths are relative to this script, so
it can be invoked from any working directory. MB is decimal; the message-size
legend uses KiB to match the actual byte counts in the CSV.
"""

from __future__ import annotations

import csv
from pathlib import Path

import matplotlib

matplotlib.use("Agg", force=True)

import matplotlib.pyplot as plt
from matplotlib.ticker import FixedLocator, NullLocator, StrMethodFormatter
import numpy as np


ROOT = Path(__file__).resolve().parent
DPI = 200
COLORS = plt.get_cmap("tab10").colors
BLUE, ORANGE, GREEN, PURPLE, GRAY = (
    COLORS[0], COLORS[1], COLORS[2], COLORS[4], COLORS[7]
)

plt.rcParams.update(
    {
        "font.family": "DejaVu Sans",
        "font.size": 11,
        "axes.labelsize": 11,
        "axes.titlesize": 13,
        "xtick.labelsize": 10,
        "ytick.labelsize": 10,
        "legend.fontsize": 10,
        "axes.axisbelow": True,
        "axes.spines.top": False,
        "axes.spines.right": False,
        "axes.linewidth": 0.8,
        "grid.alpha": 0.3,
        "grid.linewidth": 0.65,
        "lines.linewidth": 2.2,
        "lines.markersize": 6.5,
        "savefig.facecolor": "white",
        "pdf.fonttype": 42,
        "ps.fonttype": 42,
        "axes.formatter.useoffset": False,
    }
)


def read_csv(name: str, columns: tuple[str, ...], sort_by: str) -> dict[str, np.ndarray]:
    """Read and validate numeric input without depending on pandas."""
    with (ROOT / name).open(newline="", encoding="utf-8-sig") as stream:
        reader = csv.DictReader(stream)
        missing = set(columns) - set(reader.fieldnames or ())
        if missing:
            raise ValueError(f"{name}: missing columns {sorted(missing)}")
        rows = list(reader)
    if not rows:
        raise ValueError(f"{name}: no data rows")
    data = {key: np.array([float(row[key]) for row in rows]) for key in columns}
    if not all(np.isfinite(values).all() for values in data.values()):
        raise ValueError(f"{name}: non-finite values")
    order = np.argsort(data[sort_by], kind="stable")
    return {key: values[order] for key, values in data.items()}


def make_panel(fig, slot, title):
    # Reserve real space for the title and legend instead of covering the data.
    subgrid = slot.subgridspec(2, 1, height_ratios=(0.23, 1), hspace=0.025)
    header = fig.add_subplot(subgrid[0])
    header.set_axis_off()
    header.text(
        0, 1.0, title, transform=header.transAxes,
        ha="left", va="top", fontsize=13, fontweight="semibold",
    )
    ax = fig.add_subplot(subgrid[1])
    ax.grid(True, alpha=0.3)
    ax.tick_params(axis="both", which="major", pad=5)
    return ax, header


def legend_in_header(header, handles, labels=None, ncol=2):
    return header.legend(
        handles=handles,
        labels=labels,
        loc="upper left",
        bbox_to_anchor=(0, 0.63),
        ncol=ncol,
        frameon=False,
        borderaxespad=0,
        handlelength=2.5,
        handletextpad=0.65,
        columnspacing=1.25,
        labelspacing=0.55,
    )


def ecn_axis(ax, limits, ticks):
    right = ax.twinx()
    right.set_ylabel("ECN marks (k packets)", color=ORANGE, labelpad=9)
    right.set_ylim(*limits)
    right.set_yticks(ticks)
    right.tick_params(axis="y", colors=ORANGE, pad=6)
    right.spines["right"].set_visible(True)
    right.spines["right"].set_color(ORANGE)
    right.spines["top"].set_visible(False)
    right.grid(False)
    return right


def main() -> None:
    size = read_csv(
        "datasize.csv", ("dataSize_bytes", "simTime_us", "ecn_marked", "cnp_rx"),
        "dataSize_bytes",
    )
    injection = read_csv(
        "injection_rate.csv",
        ("injection_rate", "offered_gbps", "delivered_gbps", "ecn_marked"),
        "injection_rate",
    )
    messages = read_csv(
        "rate_msgsize.csv",
        ("injection_rate", "msg_bytes", "offered_gbps", "delivered_gbps", "ecn_marked"),
        "injection_rate",
    )
    nodes = read_csv(
        "nodes.csv",
        ("num_nodes", "simTime_us", "ecn_marked", "per_sender_gbps", "aggregate_gbps"),
        "num_nodes",
    )
    chunks = read_csv(
        "bulkchunk.csv", ("bulk_chunk_bytes", "simTime_us", "ecn_marked"),
        "bulk_chunk_bytes",
    )

    fig = plt.figure(figsize=(24, 13.5), facecolor="white")
    outer = fig.add_gridspec(
        2, 3, left=0.052, right=0.947, bottom=0.103, top=0.889,
        wspace=0.38, hspace=0.27,
    )
    fig.suptitle(
        "ProtoBridge sweep summary (3-node tor, 40 Gbps RoCE, DCQCN)",
        x=0.5, y=0.980, fontsize=20, fontweight="semibold",
    )
    fig.text(0.5, 0.945, "ns-3.47 ProtoBridge", ha="center", fontsize=12, color="0.30")

    panel_records = []

    # (a) Linear axes expose proportionality. Markers show measurements while
    # the dashed line shows the fitted reference, so neither line hides another.
    ax, header = make_panel(fig, outer[0, 0], "(a) Data size: linear completion")
    mb = size["dataSize_bytes"] / 1e6
    milliseconds = size["simTime_us"] / 1e3
    proportional_slope = np.dot(mb, milliseconds) / np.dot(mb, mb)
    residual_us = np.max(np.abs(milliseconds - proportional_slope * mb)) * 1e3
    reference_x = np.linspace(0, 200, 300)
    fit, = ax.plot(
        reference_x, proportional_slope * reference_x,
        color=GRAY, linestyle=(0, (5, 3)), linewidth=1.9,
        label="Proportional fit", zorder=2,
    )
    observed, = ax.plot(
        mb, milliseconds, linestyle="none", marker="o", color=BLUE,
        markeredgecolor="white", markeredgewidth=0.7, markersize=7.5,
        label="Completion time", zorder=4,
    )
    ax.set(
        xlabel="Data size (MB)", ylabel="Completion time (ms)",
        xlim=(-4, 208), ylim=(0, 90),
    )
    ax.set_xticks([0, 50, 100, 150, 200])
    ax.set_yticks([0, 15, 30, 45, 60, 75, 90])
    ax.yaxis.label.set_color(BLUE)
    ax.tick_params(axis="y", colors=BLUE)
    right = ecn_axis(ax, (0, 500), [0, 100, 200, 300, 400, 500])
    ecn, = right.plot(
        mb, size["ecn_marked"] / 1e3, color=ORANGE,
        linestyle=":", marker="s", markersize=5.5, linewidth=2.0,
        markerfacecolor="white", markeredgewidth=1.5, label="ECN marks",
    )
    ax.text(
        0.035, 0.95,
        f"Through-origin fit: {proportional_slope:.4f} ms/MB\n"
        f"Maximum residual: {residual_us:.1f} us",
        transform=ax.transAxes, va="top", fontsize=10.5,
        bbox={"facecolor": "white", "edgecolor": "none", "alpha": 0.93, "pad": 4},
    )
    legend = legend_in_header(header, [observed, fit, ecn])
    panel_records.append((ax, header, legend))

    # (b) Hollow offered markers surround the filled delivered markers when
    # their values coincide. The ECN axis has its own, clearly labeled scale.
    ax, header = make_panel(fig, outer[0, 1], "(b) Offered vs. delivered traffic")
    rate = injection["injection_rate"]
    ax.axvspan(0.8, 1.025, color=PURPLE, alpha=0.07, linewidth=0, zorder=0)
    capacity = ax.axhline(
        120, color="0.25", linestyle=(0, (6, 3)), linewidth=1.5,
        label="Aggregate line rate: 120 Gbps", zorder=1,
    )
    delivered, = ax.plot(
        rate, injection["delivered_gbps"], color=BLUE, marker="o",
        linewidth=2.5, markersize=5.8, label="Delivered", zorder=3,
    )
    offered, = ax.plot(
        rate, injection["offered_gbps"], color=GRAY, linestyle=(0, (4, 3)),
        marker="s", markerfacecolor="none", markeredgewidth=1.2,
        markersize=9, linewidth=1.6, label="Offered", zorder=4,
    )
    ax.set(
        xlabel="Injection rate (fraction of 40 Gbps / source)",
        ylabel="Goodput (Gbps)", xlim=(0.06, 1.025), ylim=(0, 132),
    )
    ax.set_xticks(np.arange(0.1, 1.01, 0.1))
    ax.xaxis.set_major_formatter(StrMethodFormatter("{x:.1f}"))
    ax.set_yticks([0, 20, 40, 60, 80, 100, 120])
    right = ecn_axis(ax, (-3, 110), [0, 20, 40, 60, 80, 100])
    ecn, = right.plot(
        rate, injection["ecn_marked"] / 1e3,
        color=ORANGE, marker="^", markersize=5.5, linestyle=":",
        markerfacecolor="white", markeredgewidth=1.3,
        label="ECN marks",
    )
    ax.text(
        0.909, 128.5, "DCQCN shaping", ha="center", va="top",
        fontsize=10, color=PURPLE,
    )
    # Order legend entries into rows of comparable width (matplotlib fills
    # legend columns first).
    legend = legend_in_header(header, [offered, capacity, delivered, ecn])
    panel_records.append((ax, header, legend))

    # (c) The gray reference is explicitly the mean offered value across the
    # three message sizes; their small offered-rate differences are preserved
    # in the input, not silently treated as identical measurements.
    ax, header = make_panel(fig, outer[0, 2], "(c) Large messages reach the knee sooner")
    message_handles = []
    for size_bytes, color, marker, marker_size, facecolor, linestyle in zip(
        sorted(np.unique(messages["msg_bytes"])),
        [BLUE, GREEN, PURPLE], ["o", "s", "^"],
        [6, 9, 10], [BLUE, "none", "none"], ["-", (0, (5, 3)), "-"],
    ):
        selected = messages["msg_bytes"] == size_bytes
        handle, = ax.plot(
            messages["injection_rate"][selected], messages["delivered_gbps"][selected],
            color=color, marker=marker, linewidth=2.4, markersize=marker_size,
            markerfacecolor=facecolor, markeredgewidth=1.5, linestyle=linestyle,
            label=f"{int(size_bytes / 1024)} KiB", zorder=3,
        )
        message_handles.append(handle)
    message_rates = np.unique(messages["injection_rate"])
    mean_offered = np.array([
        messages["offered_gbps"][messages["injection_rate"] == r].mean()
        for r in message_rates
    ])
    offered_mean, = ax.plot(
        message_rates, mean_offered, color=GRAY, linestyle=(0, (5, 3)),
        marker="D", markerfacecolor="none", markeredgewidth=1.2,
        linewidth=1.4, markersize=12, label="Offered (mean)", zorder=2,
    )
    ax.set(
        xlabel="Injection rate (fraction of 40 Gbps / source)",
        ylabel="Goodput (Gbps)", xlim=(0.465, 0.935), ylim=(0, 120),
    )
    ax.set_xticks([0.5, 0.7, 0.9])
    ax.set_yticks([0, 20, 40, 60, 80, 100, 120])
    large = messages["msg_bytes"] == 131072
    large_goodput = dict(zip(messages["injection_rate"][large], messages["delivered_gbps"][large]))
    ax.annotate(
        f"128 KiB: {large_goodput[0.7]:.1f} Gbps at 0.7\n"
        f"{large_goodput[0.9]:.1f} Gbps at 0.9",
        xy=(0.9, large_goodput[0.9]), xycoords="data",
        xytext=(0.05, 0.17), textcoords="axes fraction",
        ha="left", va="center", fontsize=10.5, color=PURPLE,
        arrowprops={"arrowstyle": "->", "color": PURPLE, "linewidth": 1.2,
                    "connectionstyle": "angle3,angleA=0,angleB=-90"},
        bbox={"facecolor": "white", "edgecolor": "none", "alpha": 0.95, "pad": 4},
    )
    legend = legend_in_header(
        header, [message_handles[0], message_handles[2], message_handles[1], offered_mean]
    )
    panel_records.append((ax, header, legend))

    # (d) num_nodes includes the receiver: the sender count is N - 1.
    ax, header = make_panel(fig, outer[1, 0], "(d) Incast throughput scales with senders")
    aggregate, = ax.plot(
        nodes["num_nodes"], nodes["aggregate_gbps"],
        color=BLUE, marker="o", label="Aggregate", markersize=7,
    )
    per_sender, = ax.plot(
        nodes["num_nodes"], nodes["per_sender_gbps"],
        color=GREEN, marker="s", label="Per sender", markersize=7,
    )
    ax.set(
        xlabel="Node count (nodes, including one receiver)",
        ylabel="Throughput (Gbps)", xlim=(1.5, 8.5), ylim=(0, 320),
    )
    ax.set_xticks(nodes["num_nodes"])
    ax.set_yticks([0, 50, 100, 150, 200, 250, 300])
    right = ecn_axis(ax, (-15, 600), [0, 150, 300, 450, 600])
    ecn, = right.plot(
        nodes["num_nodes"], nodes["ecn_marked"] / 1e3,
        color=ORANGE, linestyle=":", marker="^", markersize=6,
        markerfacecolor="white", markeredgewidth=1.5, label="ECN marks",
    )
    ax.text(
        0.035, 0.95, "Aggregate ~ 38.46 x (nodes - 1) Gbps",
        transform=ax.transAxes, va="top", fontsize=10.5,
        bbox={"facecolor": "white", "edgecolor": "none", "alpha": 0.93, "pad": 4},
    )
    ax.text(
        4.4, 54, f"{nodes['per_sender_gbps'].mean():.2f} Gbps per sender",
        fontsize=10.5, color=GREEN,
    )
    legend = legend_in_header(header, [aggregate, per_sender, ecn])
    panel_records.append((ax, header, legend))

    # (e) The completion-time scale is intentionally tight enough to resolve
    # the 3.4% change. Both y axes retain explicit, independent units.
    ax, header = make_panel(fig, outer[1, 1], "(e) Larger chunks reduce time and ECN")
    chunk_bytes = chunks["bulk_chunk_bytes"]
    chunk_ms = chunks["simTime_us"] / 1e3
    completion, = ax.plot(
        chunk_bytes, chunk_ms, color=BLUE, marker="o", markersize=7,
        label="Completion time",
    )
    ax.set_xscale("log")
    ax.set(
        xlabel="Bulk chunk size (bytes; log scale)", ylabel="Completion time (ms)",
        xlim=(900, 9300), ylim=(19.8, 21.0),
    )
    ax.xaxis.set_major_locator(FixedLocator(chunk_bytes))
    ax.set_xticklabels([f"{int(value):,}" for value in chunk_bytes])
    ax.xaxis.set_minor_locator(NullLocator())
    ax.set_yticks([19.8, 20.0, 20.2, 20.4, 20.6, 20.8, 21.0])
    ax.yaxis.set_major_formatter(StrMethodFormatter("{x:.1f}"))
    ax.yaxis.label.set_color(BLUE)
    ax.tick_params(axis="y", colors=BLUE)
    right = ecn_axis(ax, (0, 150), [0, 50, 100, 150])
    ecn, = right.plot(
        chunk_bytes, chunks["ecn_marked"] / 1e3,
        color=ORANGE, linestyle=":", marker="s", markersize=6,
        markerfacecolor="white", markeredgewidth=1.5, label="ECN marks",
    )
    time_reduction = 100 * (1 - chunk_ms[-1] / chunk_ms[0])
    ecn_reduction = 100 * (1 - chunks["ecn_marked"][-1] / chunks["ecn_marked"][0])
    ax.text(
        0.97, 0.95,
        f"Completion time: -{time_reduction:.1f}%\nECN marks: -{ecn_reduction:.1f}%",
        transform=ax.transAxes, ha="right", va="top", fontsize=10.5,
        bbox={"facecolor": "white", "edgecolor": "none", "alpha": 0.93, "pad": 4},
    )
    legend = legend_in_header(header, [completion, ecn])
    panel_records.append((ax, header, legend))

    # (f) Zoom into the delivered plateau rather than repeating the full
    # offered/delivered plot. The secondary axis is an exact unit conversion.
    ax, header = make_panel(fig, outer[1, 2], "(f) Saturation plateau, magnified")
    knee = rate >= 0.8
    plateau = rate >= 0.85
    plateau_values = injection["delivered_gbps"][plateau]
    plateau_mean = plateau_values.mean()
    plateau_utilization = 100 * plateau_mean / 120
    ax.fill_between(
        [0.85, 1.0], plateau_values.min(), plateau_values.max(),
        facecolor=PURPLE, alpha=0.10, linewidth=0,
    )
    mean_line, = ax.plot(
        [0.85, 1.0], [plateau_mean, plateau_mean],
        color=PURPLE, linestyle=(0, (5, 3)), linewidth=1.7,
        label="Mean, rate >= 0.85", zorder=2,
    )
    delivered, = ax.plot(
        rate[knee], injection["delivered_gbps"][knee],
        color=BLUE, marker="o", markersize=7,
        label="Delivered", linewidth=2.4, zorder=3,
    )
    ax.set(
        xlabel="Injection rate (fraction of 40 Gbps / source)",
        ylabel="Delivered goodput (Gbps)", xlim=(0.79, 1.01), ylim=(95.5, 97.65),
    )
    ax.set_xticks([0.8, 0.85, 0.9, 0.95, 1.0])
    ax.xaxis.set_major_formatter(StrMethodFormatter("{x:.2f}"))
    ax.set_yticks([95.5, 96.0, 96.5, 97.0, 97.5])
    ax.yaxis.set_major_formatter(StrMethodFormatter("{x:.1f}"))
    utilization = ax.secondary_yaxis(
        "right", functions=(lambda y: 100 * y / 120, lambda p: 120 * p / 100),
    )
    utilization.set_ylabel("Aggregate line-rate utilization (%)", labelpad=9)
    utilization.set_yticks([80.0, 80.5, 81.0])
    utilization.yaxis.set_major_formatter(StrMethodFormatter("{x:.1f}"))
    utilization.tick_params(axis="y", pad=6)
    utilization.spines["right"].set_visible(True)
    ax.text(
        0.035, 0.95,
        f"Plateau mean: {plateau_mean:.2f} Gbps\n"
        f"{plateau_utilization:.1f}% of aggregate line rate",
        transform=ax.transAxes, va="top", fontsize=10.5,
        bbox={"facecolor": "white", "edgecolor": "none", "alpha": 0.95, "pad": 4},
    )
    ax.text(
        0.035, 0.08,
        f"Rate 0.85-1.00: {plateau_values.min():.2f}-{plateau_values.max():.2f} Gbps",
        transform=ax.transAxes, fontsize=10,
        bbox={"facecolor": "white", "edgecolor": "none", "alpha": 0.95, "pad": 3},
    )
    legend = legend_in_header(header, [delivered, mean_line])
    panel_records.append((ax, header, legend))

    fig.text(
        0.052, 0.048,
        "ns-3.47 | Default: 3 nodes, single-leaf tor, 40 Gbps/link, 1 us latency. "
        "Node sweep varies node count. Incast panels use DCQCN.",
        ha="left", fontsize=10.5, color="0.30",
    )
    fig.text(
        0.052, 0.026,
        "Random traffic: uniform cross-node messages via SoHeader/ECN/DCQCN bulk path. "
        "MB = 1,000,000 bytes; KiB = 1,024 bytes; k packets = 1,000 packets. "
        "Chunk sweep: 50 MB.",
        ha="left", fontsize=10.5, color="0.30",
    )

    # Verify that each reserved legend fits above its plot before exporting.
    fig.canvas.draw()
    renderer = fig.canvas.get_renderer()
    figure_box = fig.bbox
    for ax, header, legend in panel_records:
        legend_box = legend.get_window_extent(renderer)
        plot_box = ax.get_window_extent(renderer)
        if legend_box.y0 <= plot_box.y1:
            raise RuntimeError("A legend intrudes into a data panel")
        if legend_box.x1 > plot_box.x1 + 2:
            raise RuntimeError("A legend exceeds its panel width")
        for artist in (legend, *header.texts):
            bounds = artist.get_window_extent(renderer)
            if not (figure_box.x0 <= bounds.x0 <= bounds.x1 <= figure_box.x1
                    and figure_box.y0 <= bounds.y0 <= bounds.y1 <= figure_box.y1):
                raise RuntimeError("A panel title or legend extends beyond the figure")

    pdf_path = ROOT / "sweep_summary.pdf"
    png_path = ROOT / "sweep_summary.png"
    fig.savefig(
        pdf_path, format="pdf", dpi=DPI,
        metadata={"Title": "ProtoBridge sweep summary",
                  "Creator": f"plot_sweeps.py / matplotlib {matplotlib.__version__} (Agg)"},
    )
    fig.savefig(png_path, format="png", dpi=DPI)
    plt.close(fig)

    print(f"matplotlib {matplotlib.__version__}; backend {matplotlib.get_backend()}; PNG dpi {DPI}")
    print(f"Proportional completion fit: {proportional_slope:.8f} ms/MB; max residual {residual_us:.3f} us")
    print(f"Saturation mean (rate >= 0.85): {plateau_mean:.5f} Gbps ({plateau_utilization:.3f}% of 120 Gbps)")
    print(f"Chunk sweep: time reduced {time_reduction:.3f}%; ECN marks reduced {ecn_reduction:.3f}%")
    for path in (pdf_path, png_path):
        if path.stat().st_size == 0:
            raise RuntimeError(f"Empty output: {path}")
        print(f"Created {path} ({path.stat().st_size:,} bytes)")


if __name__ == "__main__":
    main()
