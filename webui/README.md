# ProtoBridge Web Lab

A local browser interface for the real `gpu-cluster-sim` executable. The frontend
is plain HTML/CSS/JavaScript; the Python backend manages simulation jobs. No Node
build or frontend CDN is needed.

## Start

From the ProtoBridge repository root, with Python 3.10 or later:

```bash
python -m pip install -r webui/requirements.txt
./ns3 build gpu-cluster-sim
python -m webui --port 8080
```

Open **http://127.0.0.1:8080**. To listen on a laboratory server's network interface:

```bash
python -m webui --host 0.0.0.0 --port 8080
```

Use this interface on a trusted local/laboratory network. Internet deployment
requires authentication and stronger per-user process isolation.

## Use

1. Choose collective communication, a multi-node network, or a transfer stencil.
   The configuration selector reads all eleven shipped `.cfg` files.
2. Set rank count, application bytes per rank, and optional link overrides. The
   preview resolves the profile and `[stack]` values before the run.
3. For custom experiments, edit or import `.cfg` and `.profile` text. Advanced
   settings accept validated CLI parameters as a JSON object; the page lists the
   available parameters and ranges.
4. Run the simulation. The page polls queued/running/terminal state, displays
   the native `RESULT_START`/`RESULT_END` fields, and retains stdout/stderr.
5. Inspect step-completion timestamps, reload an experiment's inputs, compare
   2–6 completed runs, or export CSV and a complete run archive.

Each run uses independent configuration/profile snapshots and a separate process.
The source files under `configs/` are never modified. Hardware overrides are
written to `[stack]`, since profile values can override CLI hardware flags.
Stencil transfer sizes come from `transfer.*.bytes`; they need not equal the
form's `dataSize`. The P2P Ethernet/RoCE presets supply the required multi-node
fabric settings alongside the shipped stencils.

The multi-node form exposes eight inter-node topology choices. Visual form
overrides take precedence over the corresponding raw configuration keys; the
preview shows the resolved values. `seed` in the HTTP request maps to ns-3's
`RngRun` (random run number), not `RngSeed`.

The topology panel is a schematic of the selected inputs, not a packet trace.
The timing panel uses actual `stepTraceFile` records when the selected injector
produces them. A point is a completion time relative to `collectiveStartNs`;
it is not a per-step duration. Scenes without trace rows display an empty state.
Incomplete/failed/cancelled runs are excluded from completed-latency comparisons.

## Storage and execution

By default, runs are stored **outside the repository**, under
`~/.cache/protobridge-web/runs/`. Override the directory with:

```bash
python -m webui --data-dir /path/to/run-storage
```

Each run directory contains `config.cfg`, `profile.profile`, `stdout.log`,
`result.json`, and, when emitted, `steps.csv`. `result.json` records the request,
effective input summary, native results, exact argument array, Git identity,
binary SHA-256, and wall-clock time. Configuration snapshots use the absolute
profile path that was actually executed.

The service executes one simulation at a time and admits up to 16 unfinished
jobs. A run defaults to a 120-second wall-clock limit (configurable to 300).
Cancellation/timeout terminates the process group. On Linux with `prlimit`,
children also have an 8 GiB address-space limit, a 32 MiB file-size limit, and
core dumps disabled; log growth is monitored at 16 MiB on all supported hosts.
Execution uses argument arrays, with no shell. Artifact downloads are limited to
the current run's known output files. The simulator runs in its external job
directory rather than writing generated files into the checkout.

History survives service restart. Tasks interrupted by restart are marked
interrupted; they are not automatically rerun. Use **one Uvicorn worker**:
the job queue is process-local. `python -m webui` enforces this setting.
The application can also be run as `uvicorn webui.app:app --workers 1`.

Optional environment settings: `PROTOBRIDGE_WEB_REPO` selects another checkout,
and `PROTOBRIDGE_WEB_DATA_DIR` selects run storage. Equivalent command-line
options are `--repo` and `--data-dir`.

## Checks

```bash
python -m pip install -r webui/requirements-dev.txt
python -m pytest webui/tests -q
node --check webui/static/app.js
```

The API tests cover input validation, immutable snapshots, result-block parsing,
failure/incomplete status, cancellation, timeout, queue continuation, trace
semantics, restart persistence, downloads, and CSV export. They use an explicitly
fake executable to exercise exceptional paths without expensive simulations.

The HTTP API is documented at `/docs` while the server is running. The interface
does not build or modify the simulator automatically; rebuild `gpu-cluster-sim`
after changing its C++ implementation.
