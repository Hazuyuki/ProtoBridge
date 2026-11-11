/* SPDX-License-Identifier: GPL-2.0-only
 * NICS-EFC: standalone packet-level PCIe wiring example, not an RDMA workload.
 *
 * Root/DRAM -- Switch0 -- Switch1 -- NIC
 *                |          |
 *               GPU0       GPU1
 */
#include "ns3/command-line.h"
#include "ns3/pcie-fabric.h"
#include "ns3/simulator.h"

#include <cstdint>
#include <iostream>

using namespace ns3;

int
main(int argc, char* argv[])
{
    uint32_t bytes = 4096, switchNs = 100;
    uint64_t stopUs = 100000;
    CommandLine cmd(__FILE__);
    cmd.AddValue("bytes", "Bytes read from GPU0, then written to GPU1", bytes);
    cmd.AddValue("switchNs", "Illustrative forwarding delay per PCIe switch", switchNs);
    cmd.AddValue("stopUs", "Simulation deadline in microseconds", stopUs);
    cmd.Parse(argc, argv);
    NS_ABORT_MSG_IF(!bytes || bytes > (64u << 20) || switchNs > 1000000000,
                    "Use 1..64MiB and a switch delay of at most one second");
    NS_ABORT_MSG_IF(!stopUs || stopUs > 1000000000,
                    "Use a positive deadline of at most 1000 seconds");

    PcieConfig config;
    config.generation = 4;
    config.lanes = 16;
    config.switchLatency = NanoSeconds(switchNs);
    auto fabric = CreateObject<PcieFabric>();
    fabric->Configure(config);

    // Node IDs and AddLink describe the graph. The fabric routes address-based
    // requests and Requester-ID completions through the constructed graph.
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

    bool done = false;
    uint64_t completedNs = 0;
    fabric->Read(nic, gpu0, bytes, [&] {
        fabric->Write(nic, gpu1, bytes, [&] {
            done = true;
            completedNs = Simulator::Now().GetNanoSeconds();
        });
    });
    Simulator::Stop(MicroSeconds(stopUs));
    Simulator::Run(); // Also lets the finite link ACK/UpdateFC tails drain.
    const auto& stats = fabric->GetStats();
    std::cout << "status=" << (done && !fabric->HasPending() ? "complete" : "incomplete")
              << "\ncompletedNs=" << completedNs << "\nreadRequests=" << stats.readRequests
              << "\nwriteTlps=" << stats.writeTlps << "\ncompletionTlps=" << stats.completionTlps
              << "\ntlpHops=" << stats.tlpHops << '\n';
    bool ok = done && !fabric->HasPending();
    fabric->Dispose();
    Simulator::Destroy();
    return ok ? 0 : 1;
}
