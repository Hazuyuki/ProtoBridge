# ProtoBridge Web Lab

[English](README.md) · [简体中文](README.zh-CN.md)

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

Use a trusted local/laboratory network or an SSH tunnel. The service has a
shared queue and history, with no user accounts.

### Docker and SSH access

When the simulator runs in a container, start the WebUI **inside that container**
with `--host 0.0.0.0`. The browser and Python service must use the same reachable
HTTP address; the frontend needs no separate port.

| Where you run the browser | How to reach the service |
| --- | --- |
| Same machine, no container | Open `http://127.0.0.1:8080` with the default start command. |
| VS Code Remote SSH / Dev Containers | In the **Ports** panel, forward port `8080`, then open the displayed local address. On a Mac, `⌘⇧P` opens the Command Palette; F1 is unnecessary. |
| New local Docker container | Publish port `8080` when creating it, for example `docker run -p 127.0.0.1:8080:8080 …`. Start the WebUI with `--host 0.0.0.0`. |
| SSH server with port 8080 reachable on its loopback interface | Run the SSH command below **on your Mac**, then open `http://localhost:8080`. |

```bash
ssh -N -L 8080:127.0.0.1:8080 user@server
```

If SSH connects to the Docker **host** and the existing container has no
published port, forward to the container's reachable bridge IP instead:

```bash
# On the Docker host: replace CONTAINER with its name or ID.
docker inspect -f '{{range .NetworkSettings.Networks}}{{println .IPAddress}}{{end}}' CONTAINER
# On your Mac: replace CONTAINER_IP with the reported IP.
ssh -N -L 8080:CONTAINER_IP:8080 user@server
```

This bridge-IP route assumes a Linux Docker host that can reach that container.
Its IP can change after recreation. If local port 8080 is occupied, use
`-L 18080:CONTAINER_IP:8080` and open `http://localhost:18080`. Keep SSH running.

### Interface language

The header's **EN / 中文** buttons switch the interface immediately. English is
the default on a first visit, including browsers whose preferred language is
Chinese. The choice is saved in browser local storage for the same origin
(scheme, hostname and port). With storage blocked, switching works for the
current page; a new page starts in English.

Switching preserves unsaved form/source edits, selected options, the selected
run, active result tab and comparison selection. It does not submit, cancel or
restart a simulation. Labels, help, validation messages, system notices, run
states and chart captions follow the selected language. User-entered experiment
names, `.cfg` / `.profile` text, CLI keys and values, native result fields and
raw logs retain their original contents. Generated default experiment names
follow the interface language; their saved representation is English for new runs.

## Use

1. Choose collective communication, a multi-node network, or a transfer stencil.
   The configuration selector reads all eleven shipped `.cfg` files.
2. Choose the collective, algorithm, and physical topology independently. Set
   rank count, application bytes per rank, and optional link overrides. The
   preview resolves the profile, `[stack]`, and `[op]` values before the run.
3. Open the flow-control, error-injection, or recovery sections to change native
   parameters without editing JSON. For custom experiments, edit or import
   `.cfg` and `.profile` text; the advanced section still accepts validated CLI
   parameters as JSON.
4. Run the simulation. The page polls queued/running/terminal state, displays
   the native `RESULT_START`/`RESULT_END` fields, and retains stdout/stderr.
5. Inspect step-completion timestamps, reload an experiment's inputs, compare
   2–6 completed runs, or export CSV and a complete run archive.

### First experiment

Keep **Collectives → Ring AllReduce**, 8 GPUs, and 1 MiB per rank.
Click **Run simulation**. The completed run shows communication time in µs,
the exact native `simTimeNs`, packet/retry counters, and any emitted step trace.
Set the link bandwidth to **85 Gbps**, run again, then select both completed
rows and click **Compare selected**. Wall time measures execution on the host;
it is separate from simulated communication time.

For a loss experiment, choose **Multi-node network → Multi-node AllReduce**,
open **Error injection**, and click **0.1% loss + SACK**. Check the effective
settings before running, then inspect retransmissions and completion status.
To remove injected errors, click **No errors** in the same section.

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

## Optional configuration

The form reads its choices, descriptions, and numeric ranges from the backend.
Explicit form values override the corresponding source/advanced-CLI values;
blank fields retain them. The preview shows the resolved settings, including
LLR forced on by the native Ethernet/lossy-RoCE/PFC-headroom path. Saved runs
restore both operation overrides and protocol parameters.

The form uses display labels for readability and passes native identifiers to
the simulator, such as `allreduce`, `ncclring`, `leafspine`, `sack`, and `ETH`.
Changing the language does not change those values. A blank override means
**inherit**, while an explicit `0` is a value, such as disabling a mechanism.
For hardware settings, `[stack]` overrides the selected profile. CLI precedence
depends on the native execution path; the effective preview and notices show
when a profile/stack value wins. Use the corresponding form field to avoid a
conflicting extra CLI flag. Root ranks start at 0; delays use the units beside
each field, usually ns, while PFC pause time uses µs.

| Section | Choices |
| --- | --- |
| Collective | AllReduce, AllGather, ReduceScatter, AllToAll, Broadcast, Reduce; compatible Ring, Tree, Full mesh, NVLS/SHARP, Hierarchical, NCCL pipelined ring, and Auto paths |
| Physical topology | 14 native topologies, including Ring, Full mesh, NVSwitch, Fat-tree, Rail fat-tree, Leaf–spine, NVL72, Torus, Mesh, Hypercube, 2D full mesh, three-level, Dragonfly+, and Multi-plane |
| Multi-node topology | Eight inter-node choices, intra-node NVSwitch/Full mesh, and topology-specific dimensions |
| Flow control | Per-VC credits, So transmit window, VOQ depth/drop threshold, PFC static/dynamic/quanta modes and headroom, DCQCN/ECN, RTTCC, per-QP rate ceiling |
| Error injection | Uniform or per-tier BER, independent/burst errors, random packet loss, codeword and burst parameters |
| Recovery | FEC enable/scope and RS parameters, LLR enable, Go-back-N/SACK, retry count/timeout/buffer/overflow, timeout retransmission, strict So GBN |
| Link failure | Optical Leaf–spine link removal and native failure-aware routing |

The form supplies editable dimensions when selecting a topology. Torus, Mesh,
Hypercube, and 2D full mesh derive the actual rank count from those dimensions.
NVLS/SHARP require single-node NVSwitch; hierarchical algorithms require node
groups. AllToAll's non-hierarchical choices share `AlltoAllInjector` (CollNet
names are aliases). Invalid FEC tuples and PFC/ECN/drop-threshold orderings are
rejected before a process starts.

**Topology parameters** contains only structural settings for the selected
network, such as leaf/spine counts or mesh rows/columns. **Algorithm parameters**
contains operation-specific controls: Broadcast/Reduce root rank, NCCL ring
channel count, or a hierarchical operation's intra-node algorithm. These panels
are hidden when the selection needs no extra parameters. Multi-node physical
topology selectors appear directly under communication/topology.

Use **0.1% packet loss + SACK** in the error section for a small loss experiment.
It sets 4 KiB bulk packets, selective retry, a 200 µs retransmission timeout,
and sufficient credits; multi-node runs also receive a 64-chunk send window.
**No errors** clears global and tier BERs and random packet loss. Uniform BER
overrides inherited tiers; explicit tier fields can then specialize it.

`flowControl=window/rate` are unimplemented native policy seams, so the form uses
the implemented So TX window and rate machines instead. The native multi-rack
`2dfullmeshclos` builder still lacks complete routing and is not offered as a
working topology. Burst experiments require FEC off and a positive uniform BER:
the native FEC receive path uses its own independent-error decode model.

FEC enable/disable is honored by both the profile bundle and the delegated
native path. Rebuild `gpu-cluster-sim` after pulling this change.

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
node --check webui/static/options.js
node --check webui/static/i18n.js
```

The API tests cover input validation, immutable snapshots, result-block parsing,
failure/incomplete status, cancellation, timeout, queue continuation, trace
semantics, restart persistence, downloads, and CSV export. They use an explicitly
fake executable to exercise exceptional paths without expensive simulations.
`test_native_options.py` additionally checks real FEC toggling, collective
selection, optical link removal, and recovery under random loss; it is skipped
when the simulator has not been built. The native `gpu-cluster` suite also
checks the profile's FEC switch.

The HTTP API is documented at `/docs` while the server is running. The interface
does not build or modify the simulator automatically; rebuild `gpu-cluster-sim`
after changing its C++ implementation.

### Browser checks

The optional Playwright check exercises both languages against a running WebUI
and submits two real simulations. Start a separate service with an external,
empty data directory so checks do not add runs to your working history:

```bash
python -m webui --port 8081 --data-dir /tmp/protobridge-browser-runs
```

In another terminal, install the browser tools outside the checkout and run:

```bash
npm install --prefix /tmp/protobridge-browser-tools playwright
/tmp/protobridge-browser-tools/node_modules/.bin/playwright install --with-deps chromium
NODE_PATH=/tmp/protobridge-browser-tools/node_modules node webui/tests/browser_i18n.cjs http://127.0.0.1:8081
```

Node and Playwright are only needed for this optional check. They are not needed
to start or use the WebUI. Stop the temporary service after testing.

## Troubleshooting

| Symptom | Check |
| --- | --- |
| `localhost:8080` does not open over SSH | `localhost` in your Mac's browser means your Mac. Keep the SSH forwarding command running and confirm its destination reaches the service. |
| Works inside the container, fails through forwarding | Start with `--host 0.0.0.0`; confirm the tunnel targets the container's address or a published host port. |
| Simulator not ready | Run `./ns3 build gpu-cluster-sim` in the selected checkout. The WebUI does not build C++ automatically. |
| Dropdowns stay on “Loading…” | Check the connection message and browser console, then reload. HTML and assets revalidate on each visit, and asset URLs include content hashes. |
| Cannot load translations | Reload once the service is reachable. The page falls back to English and disables language buttons until the catalog can load. |
| Language is different on another URL | Preferences belong to an origin: `localhost:8080`, `127.0.0.1:8080` and `localhost:18080` have separate storage. |
| Cannot run or compare a scenario | Read the validation message. Only completed runs can be compared; incomplete delivery has no completed-operation latency. |
| Form choices do not appear to affect a run | Check effective settings, notices and the saved input snapshot. Some native paths force LLR or take profile/stack values before CLI flags. |

## HTTP API and translations

| Endpoint | Purpose |
| --- | --- |
| `GET /api/catalog` | Repository templates, native choices and field ranges |
| `GET /api/status` | Binary readiness, source revision and queue counts |
| `POST /api/preview` | Validate inputs and resolve effective settings without executing |
| `POST /api/runs` | Submit a run; returns HTTP 202 and its ID |
| `GET /api/runs` / `GET /api/runs/{id}` | List history / inspect a run |
| `POST /api/runs/{id}/cancel` | Cancel a queued or active run |
| `GET /api/runs/{id}/trace` / `log` | Step timestamps / raw output (`offset` supports incremental log reads) |
| `GET /api/runs/{id}/artifacts/{filename}` | Download a snapshot, result, trace, log or `run.zip` |
| `GET /api/runs/export.csv` | Export the shared history |

API metadata and application errors use English regardless of browser language;
the browser localizes their presentation. Status codes, native parameter names,
result keys and artifact contents do not change with the language. Run responses
include `name_is_default` so clients can translate generated names while leaving
user-entered names intact. Older saved Chinese system messages are translated
for display without rewriting their files.

`static/locales.json` is the shared English/Chinese message catalog. English
phrases are message IDs; `aliases` recognizes the previous Chinese metadata and
saved system messages. `static/i18n.js` applies static `data-i18n` bindings and
formats dynamic messages. `webui/i18n.py` canonicalizes API-owned presentation
text. Add entries in both languages with matching `{placeholder}` names; keep
interpolated parameter values and source/log text out of translation. Add static
bindings or call `t()` for new UI messages, then run the catalog and browser
checks. Asset hashes cover both the translation script and its JSON catalog.
