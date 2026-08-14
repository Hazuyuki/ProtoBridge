/*
 * SPDX-License-Identifier: GPL-2.0-only
 * Author: GPU Cluster Simulator Team
 *
 * Example demonstrating hybrid multi-fabric GPU cluster topology
 *
 * This example creates a 2-node GPU cluster with:
 * - Node 0: 4 GPUs connected via NVLink (ranks 0-3)
 * - Node 1: 4 GPUs connected via NVLink (ranks 4-7)
 * - Ethernet link connecting GPU 3 (gateway) to GPU 4 (gateway)
 *
 * Traffic flows tested:
 * - Within-fabric: GPU 0 -> GPU 2 (within Node 0)
 * - Cross-fabric: GPU 0 -> GPU 6 (via gateway)
 */

#include "ns3/core-module.h"
#include "ns3/gpu-cluster-module.h"
#include "ns3/point-to-point-module.h"

#include <iostream>

using namespace ns3;

NS_LOG_COMPONENT_DEFINE("HybridClusterExample");

int
main(int argc, char* argv[])
{
    // Enable logging
    LogComponentEnable("HybridClusterExample", LOG_LEVEL_INFO);

    // Configuration parameters
    uint32_t gpusPerNode = 4;
    std::string nvlinkRate = "300Gbps";
    std::string nvlinkDelay = "500ns";
    std::string ethernetRate = "100Gbps";
    std::string ethernetDelay = "10us";

    CommandLine cmd;
    cmd.AddValue("gpusPerNode", "Number of GPUs per node", gpusPerNode);
    cmd.Parse(argc, argv);

    NS_LOG_UNCOND("=== Hybrid GPU Cluster Example ===");
    NS_LOG_UNCOND("Configuration:");
    NS_LOG_UNCOND("  GPUs per node: " << gpusPerNode);
    NS_LOG_UNCOND("  NVLink: " << nvlinkRate << ", " << nvlinkDelay);
    NS_LOG_UNCOND("  Ethernet: " << ethernetRate << ", " << ethernetDelay);

    // Create hybrid topology helper
    HybridTopologyHelper helper;

    // Add NVLink fabric for Node 0 (GPUs 0-3)
    NS_LOG_UNCOND("\nAdding NVLink fabric for Node 0 (ranks 0-" << gpusPerNode - 1 << ")");
    helper.AddNvLinkFabric(gpusPerNode, "fullmesh", 0, nvlinkRate, nvlinkDelay);

    // Add NVLink fabric for Node 1 (GPUs 4-7)
    NS_LOG_UNCOND("Adding NVLink fabric for Node 1 (ranks " << gpusPerNode << "-" << 2 * gpusPerNode - 1 << ")");
    helper.AddNvLinkFabric(gpusPerNode, "fullmesh", gpusPerNode, nvlinkRate, nvlinkDelay);

    // Add Ethernet link between gateways (last GPU in each node)
    uint16_t gateway1Rank = gpusPerNode - 1;  // GPU 3 in Node 0
    uint16_t gateway2Rank = gpusPerNode;      // GPU 4 in Node 1
    NS_LOG_UNCOND("Adding cross-fabric link: rank " << gateway1Rank << " <-> rank " << gateway2Rank);
    helper.AddCrossFabricLink(0, 1, gateway1Rank, gateway2Rank, ethernetRate, ethernetDelay);

    // Build topology
    NS_LOG_UNCOND("\nBuilding topology...");
    NodeContainer allNodes = helper.Build();
    NS_LOG_UNCOND("Created " << allNodes.GetN() << " nodes across " << helper.GetNFabrics() << " fabrics");
    NS_LOG_UNCOND("Total GPUs: " << helper.GetTotalNumGpus());

    // Populate cross-fabric routing
    NS_LOG_UNCOND("\nPopulating cross-fabric routing tables...");
    helper.PopulateCrossFabricRouting();

    // Get endpoints
    ApplicationContainer endpoints = helper.GetAllEndpoints();
    NS_LOG_UNCOND("Created " << endpoints.GetN() << " endpoint applications");

    // Print endpoint information
    for (uint32_t i = 0; i < endpoints.GetN(); ++i)
    {
        Ptr<FabricEndpoint> ep = DynamicCast<FabricEndpoint>(endpoints.Get(i));
        if (ep)
        {
            NS_LOG_UNCOND("  Rank " << ep->GetRank()
                          << ": fabric=" << static_cast<int>(ep->GetFabricType())
                          << ", devices=" << ep->GetNNetDevices());
        }
    }

    Simulator::Stop(Seconds(0.1));
    Simulator::Run();
    Simulator::Destroy();

    NS_LOG_UNCOND("\nSimulation complete.");

    return 0;
}
