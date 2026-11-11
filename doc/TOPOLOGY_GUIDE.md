# Build a custom topology

Start with a small graph and one working transfer, then add the paths and
resources needed by your workload. This guide follows two complete C++ examples:

- [custom-network-topology.cc](../src/gpu-cluster/examples/custom-network-topology.cc):
  four RoCE endpoints, two Ethernet switches, explicit routes and concurrent
  RDMA Writes.
- [custom-pcie-topology.cc](../src/gpu-cluster/examples/custom-pcie-topology.cc):
  an explicit local PCIe graph, followed by asynchronous reads and writes.

The network example checks byte delivery and source completion. The PCIe
example checks completion and drains link feedback. Both return a nonzero exit
status if the simulation deadline is reached before the checks pass.

The rates, delays and resource sizes are example inputs. They are not a
calibration of a GPU, NIC or PCIe switch.

## Choose the right layer

| Requirement | Entry point |
|---|---|
| Change an existing fabric's size or shape | Use its CLI parameters or WebUI controls. |
| Define a new network connection graph | Wire `Node`/`NetDevice` objects and populate fabric routes, as in the network example. |
| Define a local GPU/NIC PCIe graph | Use `PcieFabric::AddRootComplex`, `AddSwitch`, `AddEndpoint` and `AddLink`. |
| Define a new communication schedule | Use a transfer stencil or injector; this changes traffic, not physical wiring. |

For a single-node fabric, `--topology` selects a
`GpuClusterTopologyHelper` builder. In multi-node mode,
`--intraNodeTopology` and `--interNodeTopology` configure
`MultiNodeTopologyHelper`. `--pcieTopology` selects the RDMA runtime's local
PCIe template.

The main simulator and WebUI currently select built-in templates. They do not
import an arbitrary node/link JSON file or offer a graphical wiring editor.
A standalone C++ example can build a different graph without adding a new
main-simulator option.

## Build and run

Run these commands from the repository root after the
[installation steps](../README.md#quickstart):

~~~bash
./ns3 configure --enable-modules=gpu-cluster --enable-examples -d debug
./ns3 build custom-network-topology custom-pcie-topology -j 2
./ns3 run "custom-network-topology" --no-build
./ns3 run "custom-pcie-topology" --no-build
~~~

Reconfigure after registering a new CMake example so the `ns3` wrapper discovers
its target. To build it as part of an existing checkout, keep that checkout's
chosen module/test options when configuring.

These programs use simulated devices and memories. No GPU or RDMA driver is
required.

## Network example: two leaves and a shared link

~~~text
GPU/NIC 0 ---\                  /--- GPU/NIC 2
              Leaf 0 -- Leaf 1
GPU/NIC 1 ---/                  \--- GPU/NIC 3

access links: 200 Gb/s each
shared leaf-to-leaf link: 100 Gb/s
~~~

Each endpoint represents a different host with one GPU/NIC pair. There is no
intra-host NVLink fabric in this example. Two flows, `0 -> 2` and `1 -> 3`,
share the middle link. `--reverse=1` uses `2 -> 0` and `3 -> 1` instead.

The numbered comments in the source follow the construction order below.

### 1. Create nodes and switch devices

~~~cpp
NodeContainer hosts, switchNodes;
hosts.Create(4);
switchNodes.Create(2);

NvSwitchHelper switchHelper;
switchHelper.SetSwitchType("ns3::EthernetSwitch");
~~~

A host node receives a `FabricEndpoint` application. A switch node receives an
`EthernetSwitch` device through `switchHelper.Install(...)`.

The example disables ECN marking and configured tail-drop, and gives queues
enough capacity for its default workload. Queue capacity remains finite; this
setup is not a congestion-control experiment.

### 2. Give endpoints identities and a protocol

Each endpoint needs a unique rank and an appropriate local-rank range:

~~~cpp
ep->SetRank(rank);
ep->SetLocalRankRange(rank, 1);
ep->SetFabricType(FabricType::ROCE);
~~~

Here, a host contains only its own rank, so every other rank is remote.
For several GPUs on one host, give those endpoints the same host-local rank
range. The range affects local/remote dispatch; it does not create links.

Install a `RoceProtocolModel` and `RocePayloadBuilder`, add the endpoint to its
host and set its application start time, as shown in the source. Launch delay
and extra device costs are left at zero for this example. The reorder buffer
is bypassed because the graph has a single route between each communicating
pair.

Keep these identifiers separate:

| Identifier | Meaning in the example |
|---|---|
| Node ID | ns-3's identity for a host or switch node. |
| Rank | The communication endpoint's identity. |
| MAC address | The final destination used by switch forwarding. |
| Device index | An endpoint's index returned by `AddNetDevice`. |
| Port index | A switch's index returned by `AddPort`. |

Some values happen to coincide in a small graph. Do not rely on that when
changing construction order or adding links.

### 3. Install links and retain their port IDs

`MakeLink` supplies a point-to-point rate, propagation delay, device MTU and
transmit queue. Separate helpers allow different rates for access and shared
links.

~~~cpp
auto devices = access.Install(hosts.Get(rank), switchNodes.Get(side));
endpointDevices[rank] = endpoints[rank]->AddNetDevice(devices.Get(0));
localPorts[rank] = leaves[side]->AddPort(devices.Get(1));
macs[rank] = Mac48Address::ConvertFrom(devices.Get(0)->GetAddress());
~~~

`Install` creates a device at each end of the link. Register the host-side
device with its endpoint and the switch-side device with the switch. Store
the returned IDs before constructing routes.

For the middle link, both ends belong to switches:

~~~cpp
auto middle = trunk.Install(switchNodes.Get(0), switchNodes.Get(1));
auto leftPort = leaves[0]->AddPort(middle.Get(0));
auto rightPort = leaves[1]->AddPort(middle.Get(1));
~~~

The example's point-to-point MTU of 65535 B leaves room for the modeled frame
headers. `RdmaConfig::mtuBytes` separately limits network packet bodies to
4096 B. Neither is the application Write size.

### 4. Populate endpoint and switch routes

Endpoints select an outgoing device by destination rank. They also need the
final peer's MAC address:

~~~cpp
endpoints[rank]->SetRoutingEntry(peer, endpointDevices[rank]);
endpoints[rank]->SetNeighborMac(peer, macs[peer]);
~~~

Every endpoint in this graph has one network device. A multi-homed endpoint
must select the appropriate device or device set for each destination.

Switches forward by the final destination MAC:

| Switch | Destination ranks 0/1 | Destination ranks 2/3 |
|---|---|---|
| Leaf 0 | The corresponding local access port | The middle link |
| Leaf 1 | The middle link | The corresponding local access port |

The complete route loop uses the stored IDs:

~~~cpp
for (uint32_t side = 0; side < 2; ++side)
{
    for (uint16_t dest = 0; dest < 4; ++dest)
    {
        uint32_t port = dest / 2 == side ? localPorts[dest] : trunkPorts[side];
        leaves[side]->AddStaticRoute(macs[dest], port);
    }
}
~~~

Install routes in both directions. Source-side RDMA completion depends on
returning ACKs even when application data travels in only one direction.
These fabric routes use `SetRoutingEntry` and `AddStaticRoute`; IPv4 routing
tables do not supply them.

When adding a new endpoint, update its identity, link attachment, MAC mapping
and routes on every switch along its path.

### 5. Attach RDMA resources and submit work

Once devices and routes exist, configure and attach a submission runtime to
each endpoint:

~~~cpp
auto submission = CreateObject<RdmaSubmissionModel>();
submission->Configure(rdma);
submission->SetHostMemoryResources(
    std::make_shared<RdmaHostMemoryResources>());
ep->SetRdmaSubmissionModel(submission);
ep->Initialize();
~~~

A separate host-memory resource object is appropriate here because each
endpoint represents a separate host. If several endpoints share a host,
share one `RdmaHostMemoryResources` among them before submitting work.

The example uses CPU initiation and GPUDirect payloads, with one QP per peer
and a 16-WR limit per QP.
`--pcieModel=bandwidth` selects the default bandwidth path;
`--pcieModel=tlp` enables the existing per-host PCIe packet template. No fitted
CPU posting or completion delay is loaded.

`SubmitRdmaWrite` sends the timing-only bulk Write workload. For addressed
buffers and data-content checks, use the
[RDMA operation API](RDMA.md#addressed-operations).

The receive callback counts `FabricHeader::GetEffectiveDataSize()` rather
than packet size, because headers and framing are not application bytes.
The source callback counts completed Writes. `Progress::Check` stops only
after both counts reach their expected totals.

### Change the example and inspect results

| Flag | Default | Meaning |
|---|---|---|
| `--accessRate` | `200Gbps` | Rate of each endpoint-to-leaf link. |
| `--trunkRate` | `100Gbps` | Rate of the shared leaf-to-leaf link. |
| `--linkLatencyNs` | `0` | Propagation delay on each network link, in ns. |
| `--switchDelayNs` | `649` | Forwarding delay on each network switch, in ns. |
| `--dataSize` | `65536` | Application bytes per Write. |
| `--iterations` | `4` | Writes per flow. |
| `--reverse` | `0` | Reverse both flows. |
| `--pcieModel` | `bandwidth` | Local PCIe timing model: `bandwidth` or `tlp`. |
| `--stopUs` | `100000` | Simulation deadline, in microseconds. |

~~~bash
./ns3 run "custom-network-topology --reverse=1" --no-build
./ns3 run "custom-network-topology --trunkRate=200Gbps" --no-build
./ns3 run "custom-network-topology --pcieModel=tlp" --no-build
~~~

The default sends two flows with four 64-KiB Writes each. A successful run
reports:

~~~text
expectedBytes=524288
receivedBytes=524288
completedWrites=8
status=complete
~~~

`durationNs` measures first submission to completion of both checks. On the
recorded default run it was 45301 ns; increasing only the middle link to
200 Gb/s reduced it to 24052 ns. These numbers describe this example and its
model inputs, not hardware accuracy. `aggregate_MBps` counts the sum of both
flows' application bytes per duration, in decimal MB/s.

A deliberately short deadline exercises the incomplete result and nonzero
exit status:

~~~bash
./ns3 run "custom-network-topology --stopUs=1" --no-build
~~~

For a larger experiment, increase `--stopUs` if needed and check queue
capacity and completion counters. Do not treat a reached deadline as
successful delivery.

## PCIe example: connect GPU and NIC endpoints explicitly

~~~text
Root/DRAM -- Switch0 -- Switch1 -- NIC
                |          |
               GPU0       GPU1
~~~

The [PCIe source](../src/gpu-cluster/examples/custom-pcie-topology.cc) configures
one `PcieFabric`, creates the nodes, then supplies all edges:

~~~cpp
auto root = fabric->AddRootComplex("root/host-dram");
auto s0 = fabric->AddSwitch("switch0");
auto s1 = fabric->AddSwitch("switch1");
auto gpu0 = fabric->AddEndpoint("gpu0-hbm");
auto gpu1 = fabric->AddEndpoint("gpu1-hbm");
auto nic = fabric->AddEndpoint("nic");

fabric->AddLink(root, s0);
fabric->AddLink(s0, s1);
fabric->AddLink(gpu0, s0);
fabric->AddLink(gpu1, s1);
fabric->AddLink(nic, s1);
~~~

Build the complete graph before issuing `Read` or `Write`. The component
routes memory requests by address and returned Completions by Requester ID;
this graph does not use the network example's rank/MAC route tables.

Next, issue a read from GPU0 followed by a write to GPU1:

~~~cpp
fabric->Read(nic, gpu0, bytes, [&] {
    fabric->Write(nic, gpu1, bytes, [&] {
        done = true;
        completedNs = Simulator::Now().GetNanoSeconds();
    });
});
~~~

`AddEndpoint` and `AddRootComplex` also accept read/write memory-service
callbacks that return a nonnegative delay. The example leaves those callbacks
empty; no additional HBM/DRAM service time is assumed.

~~~bash
./ns3 run "custom-pcie-topology --bytes=4096 --switchNs=100" --no-build
~~~

| Flag | Default | Meaning |
|---|---|---|
| `--bytes` | `4096` | Bytes read from GPU0, then written to GPU1. |
| `--switchNs` | `100` | Illustrative forwarding delay per PCIe switch. |
| `--stopUs` | `100000` | Simulation deadline, in microseconds. |

The default aligned transfer, with MPS 256 B and MRRS 512 B, creates eight
MRd requests, sixteen CplD responses and sixteen MWr packets. The read request
and response paths traverse three links; the write path traverses two. That
gives `8*3 + 16*3 + 16*2 = 104` TLP hops.

`completedNs` records the read-plus-write callback time. The simulator also
drains link ACK and credit-update events; the final simulation clock can
therefore be later. Success requires `PcieFabric::HasPending()` to be false.
This program models transfer timing, not copied memory values or an RDMA
network operation.

All links in one `PcieFabric` share the configured Gen/lane/resource settings.
Changing connectivity does not provide per-link mixed-generation settings.
See [PCIe execution and scope](PCIE.md#packet-execution) for the packet model.

## How these examples connect to the main simulator

The two programs are independent executables:

- The network example can select the RDMA runtime's existing local PCIe
  template with `--pcieModel=tlp`.
- The standalone PCIe graph is not automatically bound to that runtime.
  Custom GPU/NIC placements require an integration change in
  [`RdmaSubmissionModel::EnsurePcie`](../src/gpu-cluster/model/rdma-submission-pcie.cc)
  and an explicit endpoint binding. Adding edges to a standalone object
  does not change the main simulator's host topology.

To expose a new network family as a main-simulator option:

1. Move reusable graph construction into
   [`GpuClusterTopologyHelper`](../src/gpu-cluster/helper/gpu-cluster-helper.h)
   for a single-node fabric, or
   [`MultiNodeTopologyHelper`](../src/gpu-cluster/helper/multi-node-topology-helper.h)
   for a multi-node network.
2. Create links, register the correct local/remote devices and populate
   endpoint and switch routes. In the multi-node helper, review both
   `BuildInterNodeLinks` and `PopulateInterNodeRouting`.
3. Add the selection to
   [`gpu-cluster-sim.cc`](../scratch/gpu-cluster-sim.cc), including help,
   validation and shape parameters. Update profile/config handling if those
   inputs should be configurable there.
4. If collective injectors use this graph, supply the ring/tree neighbor
   mapping through `SetRingNeighbors` / `SetTreeNeighbors` or the helper's
   existing `InstallCollectiveEmbedding` interface. Validate the intended
   collective; a successful P2P Write does not establish collective support.
   Multicast/in-switch aggregation also needs its switch-specific mappings.
5. For WebUI selection, update the supported choices and shape fields in
   [`webui/options.py`](../webui/options.py), their
   [translations](../webui/static/locales.json) and relevant UI checks.
6. Add an example command and focused verification for the new family.

## Register another C++ example

Long-lived C++ API examples belong in `src/gpu-cluster/examples/`. Add their
build target to its [CMakeLists.txt](../src/gpu-cluster/examples/CMakeLists.txt):

~~~cmake
build_lib_example(
  NAME my-topology-example
  SOURCE_FILES my-topology-example.cc
  LIBRARIES_TO_LINK ${libgpu-cluster}
                    ${libpoint-to-point}
                    ${libnetwork}
                    ${libcore}
)
~~~

Reconfigure and build the target before running it:

~~~bash
./ns3 configure --enable-modules=gpu-cluster --enable-examples -d debug
./ns3 build my-topology-example -j 2
./ns3 run "my-topology-example" --no-build
~~~

## Verification and common problems

The shipped examples check their own delivery/completion conditions. CI runs
the forward network, reverse network, network with TLP, and standalone PCIe
cases. When changing the graph, also compare a known bottleneck or path change.

| Symptom | Check |
|---|---|
| No payload arrives | Endpoint ranks, final peer MACs, attached devices and switch routes. |
| Payload arrives but source work stays pending | Reverse ACK routing and source completion observation. |
| Only some destinations work | Every new rank needs a path through each switch on its route. |
| Duplicate bytes | Accidental flooding, duplicate links or unintended forwarding paths. |
| Unexpected link sharing | Physical output ports and shared-link rates, not just workload flow IDs. |
| Run reaches `stopUs` | Workload size, offered load, finite queues and missing forward/return paths. |
| New example target is unknown | CMake registration, examples enabled, and reconfiguration. |
| CLI/WebUI rejects the new name | The standalone example has not been registered as a main-simulator topology. |
| Custom PCIe wiring does not affect RDMA | The graph has not been bound to the RDMA runtime. |

Use [Configuration](CONFIG_GUIDE.md) for input precedence, [Architecture](ARCHITECTURE.md)
for packet execution and [RDMA](RDMA.md) for operations, resources and completion
semantics. Those guides also define parameters and record calibration provenance.
