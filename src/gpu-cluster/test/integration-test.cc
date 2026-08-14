/*
 * SPDX-License-Identifier: GPL-2.0-only
 * Author: GPU Cluster Simulator Team
 *
 * Integration tests for GPU cluster module
 */

#include "ns3/test.h"
#include "ns3/fabric-header.h"
#include "ns3/fabric-endpoint.h"
#include "ns3/nvswitch.h"
#include "ns3/credit-manager.h"
#include "ns3/config.h"
#include "ns3/reorder-buffer.h"
#include "ns3/link-degradation.h"
#include "ns3/p2p-traffic.h"
#include "ns3/gpu-cluster-helper.h"
#include "ns3/packet.h"
#include "ns3/mac48-address.h"
#include "ns3/simulator.h"
#include "ns3/node.h"
#include "ns3/point-to-point-helper.h"
#include "ns3/point-to-point-net-device.h"
#include "ns3/uinteger.h"
#include "ns3/double.h"
#include "ns3/string.h"
#include "ns3/random-variable-stream.h"

using namespace ns3;

static void
DisableUnconfiguredLinkErrors()
{
    Config::SetDefault("ns3::LinkDegradationModel::Ber", DoubleValue(0.0));
}

/**
 * @brief Integration test for Ring topology with routing
 */
class RingTopologyTest : public TestCase
{
  public:
    RingTopologyTest();
    ~RingTopologyTest() override;

    void DoRun() override;

  private:
    void TestRingBuild();
    void TestRingRouting();
    void TestRingPacketDelivery();
    uint32_t m_receivedCount;
    uint16_t m_lastReceivedRank;
    void OnPacketReceived(uint16_t rank, Ptr<Packet> packet, FabricHeader header);
};

RingTopologyTest::RingTopologyTest()
    : TestCase("RingTopology"),
      m_receivedCount(0),
      m_lastReceivedRank(0xFFFF)
{
}

RingTopologyTest::~RingTopologyTest()
{
}

void
RingTopologyTest::OnPacketReceived(uint16_t rank, Ptr<Packet> packet, FabricHeader header)
{
    m_receivedCount++;
    // The rank parameter is the receiver's local rank, use header for source
    m_lastReceivedRank = header.GetSourceRank();
}

void
RingTopologyTest::DoRun()
{
    DisableUnconfiguredLinkErrors();
    TestRingBuild();
    TestRingRouting();
    TestRingPacketDelivery();
}

void
RingTopologyTest::TestRingBuild()
{
    GpuClusterTopologyHelper helper(4, 0);  // 4 GPUs, 0 switches
    helper.SetLinkDataRate("100Gbps");
    helper.SetLinkDelay("500ns");

    NodeContainer nodes = helper.BuildRing();

    // Check that 4 nodes were created
    NS_TEST_EXPECT_MSG_EQ(nodes.GetN(), 4, "Ring should have 4 nodes");

    // Check endpoints
    ApplicationContainer endpoints = helper.GetEndpoints();
    NS_TEST_EXPECT_MSG_EQ(endpoints.GetN(), 4, "Should have 4 endpoints");

    // Check each endpoint has 2 NetDevices (left and right neighbors)
    for (uint32_t i = 0; i < 4; ++i)
    {
        Ptr<FabricEndpoint> ep = DynamicCast<FabricEndpoint>(endpoints.Get(i));
        NS_TEST_EXPECT_MSG_EQ(ep->GetNNetDevices(), 2, "Each endpoint should have 2 NetDevices");
        NS_TEST_EXPECT_MSG_EQ(ep->GetRank(), i, "Rank should match index");
    }

    // No switches in ring topology
    NodeContainer switches = helper.GetSwitchNodes();
    NS_TEST_EXPECT_MSG_EQ(switches.GetN(), 0, "Ring topology should have no switches");
}

void
RingTopologyTest::TestRingRouting()
{
    GpuClusterTopologyHelper helper(4, 0);  // 4 GPUs, 0 switches
    NodeContainer nodes = helper.BuildRing();
    ApplicationContainer endpoints = helper.GetEndpoints();

    // Test routing table entries
    // In a 4-node ring, each node should have routes to 3 other nodes
    for (uint32_t src = 0; src < 4; ++src)
    {
        Ptr<FabricEndpoint> epSrc = DynamicCast<FabricEndpoint>(endpoints.Get(src));

        // Check routing entries exist for all destinations
        for (uint32_t dst = 0; dst < 4; ++dst)
        {
            if (src == dst)
                continue;

            uint32_t devIdx = epSrc->GetRoutingDeviceIndex(dst);
            NS_TEST_EXPECT_MSG_EQ(devIdx < 2, true, "Routing device index should be valid (0 or 1)");

            // In ring topology, neighbor MAC is only set for immediate neighbors
            // Check that immediate neighbors (left and right) have MAC addresses
            uint32_t cwDist = (dst > src) ? (dst - src) : (4 - src + dst);
            uint32_t ccwDist = 4 - cwDist;
            if (cwDist == 1 || ccwDist == 1)  // Direct neighbor
            {
                Mac48Address neighborMac = epSrc->GetNeighborMac(dst);
                NS_TEST_EXPECT_MSG_EQ(neighborMac.IsBroadcast(), false, "Neighbor MAC should be set for direct neighbor");
            }
        }
    }
}

void
RingTopologyTest::TestRingPacketDelivery()
{
    GpuClusterTopologyHelper helper(4, 0);  // 4 GPUs, 0 switches
    helper.SetLinkDataRate("100Gbps");
    helper.SetLinkDelay("500ns");
    NodeContainer nodes = helper.BuildRing();
    ApplicationContainer endpoints = helper.GetEndpoints();

    // Set up receive callback on node 1 (immediate neighbor of node 0)
    Ptr<FabricEndpoint> ep1 = DynamicCast<FabricEndpoint>(endpoints.Get(1));
    m_receivedCount = 0;
    ep1->SetReceiveCallback(MakeCallback(&RingTopologyTest::OnPacketReceived, this));

    // Initialize credits for all VCs
    for (uint32_t i = 0; i < 4; ++i)
    {
        Ptr<FabricEndpoint> ep = DynamicCast<FabricEndpoint>(endpoints.Get(i));
        ep->SetNumVirtualChannels(1);
        ep->SetVcCredits(0, 1000);
    }

    // Send data from node 0 to node 1 (direct neighbor in ring)
    Ptr<FabricEndpoint> ep0 = DynamicCast<FabricEndpoint>(endpoints.Get(0));
    uint8_t data[] = {0x01, 0x02, 0x03, 0x04, 0x05};
    ep0->SendData(1, data, sizeof(data), 1, 0);

    // Run simulation
    Simulator::Stop(MicroSeconds(100));
    Simulator::Run();

    // Check that packet was received
    NS_TEST_EXPECT_MSG_EQ(m_receivedCount >= 1, true, "Packet should be received");
    NS_TEST_EXPECT_MSG_EQ(m_lastReceivedRank, 0, "Source rank should be 0");

    Simulator::Destroy();
}

/**
 * @brief Integration test for FullMesh topology
 */
class FullMeshTopologyTest : public TestCase
{
  public:
    FullMeshTopologyTest();
    ~FullMeshTopologyTest() override;

    void DoRun() override;

  private:
    void TestFullMeshBuild();
    void TestFullMeshRouting();
    void TestFullMeshAllToAll();
    uint32_t m_receivedCount[4];
    void OnPacketReceived(uint16_t rank, Ptr<Packet> packet, FabricHeader header);
};

FullMeshTopologyTest::FullMeshTopologyTest()
    : TestCase("FullMeshTopology")
{
    for (uint32_t i = 0; i < 4; ++i)
        m_receivedCount[i] = 0;
}

FullMeshTopologyTest::~FullMeshTopologyTest()
{
}

void
FullMeshTopologyTest::OnPacketReceived(uint16_t rank, Ptr<Packet> packet, FabricHeader header)
{
    uint16_t destRank = header.GetDestRank();
    if (destRank < 4)
        m_receivedCount[destRank]++;
}

void
FullMeshTopologyTest::DoRun()
{
    DisableUnconfiguredLinkErrors();
    TestFullMeshBuild();
    TestFullMeshRouting();
    TestFullMeshAllToAll();
}

void
FullMeshTopologyTest::TestFullMeshBuild()
{
    GpuClusterTopologyHelper helper(4, 0);  // 4 GPUs, 0 switches
    helper.SetLinkDataRate("100Gbps");
    helper.SetLinkDelay("500ns");

    NodeContainer nodes = helper.BuildFullMesh();

    // Check that 4 nodes were created
    NS_TEST_EXPECT_MSG_EQ(nodes.GetN(), 4, "FullMesh should have 4 nodes");

    // Check endpoints
    ApplicationContainer endpoints = helper.GetEndpoints();
    NS_TEST_EXPECT_MSG_EQ(endpoints.GetN(), 4, "Should have 4 endpoints");

    // Check each endpoint has N-1 = 3 NetDevices (one per peer)
    for (uint32_t i = 0; i < 4; ++i)
    {
        Ptr<FabricEndpoint> ep = DynamicCast<FabricEndpoint>(endpoints.Get(i));
        NS_TEST_EXPECT_MSG_EQ(ep->GetNNetDevices(), 3, "Each endpoint should have 3 NetDevices");
        NS_TEST_EXPECT_MSG_EQ(ep->GetRank(), i, "Rank should match index");
    }

    // No switches in full-mesh topology
    NodeContainer switches = helper.GetSwitchNodes();
    NS_TEST_EXPECT_MSG_EQ(switches.GetN(), 0, "FullMesh topology should have no switches");
}

void
FullMeshTopologyTest::TestFullMeshRouting()
{
    GpuClusterTopologyHelper helper(4, 0);  // 4 GPUs, 0 switches
    NodeContainer nodes = helper.BuildFullMesh();
    ApplicationContainer endpoints = helper.GetEndpoints();

    // Test routing table entries
    // In full-mesh, each node has direct link to every other node
    for (uint32_t src = 0; src < 4; ++src)
    {
        Ptr<FabricEndpoint> epSrc = DynamicCast<FabricEndpoint>(endpoints.Get(src));

        for (uint32_t dst = 0; dst < 4; ++dst)
        {
            if (src == dst)
                continue;

            uint32_t devIdx = epSrc->GetRoutingDeviceIndex(dst);
            NS_TEST_EXPECT_MSG_EQ(devIdx < 3, true, "Routing device index should be valid (0, 1, or 2)");

            // Verify neighbor MAC is set
            Mac48Address neighborMac = epSrc->GetNeighborMac(dst);
            NS_TEST_EXPECT_MSG_EQ(neighborMac.IsBroadcast(), false, "Neighbor MAC should be set");
        }
    }
}

void
FullMeshTopologyTest::TestFullMeshAllToAll()
{
    GpuClusterTopologyHelper helper(4, 0);  // 4 GPUs, 0 switches
    helper.SetLinkDataRate("100Gbps");
    helper.SetLinkDelay("500ns");
    NodeContainer nodes = helper.BuildFullMesh();
    ApplicationContainer endpoints = helper.GetEndpoints();

    // Set up receive callbacks and credits for each VC on each endpoint
    for (uint32_t i = 0; i < 4; ++i)
    {
        Ptr<FabricEndpoint> ep = DynamicCast<FabricEndpoint>(endpoints.Get(i));
        ep->SetNumVirtualChannels(1);
        // Each endpoint needs credits for sending to 3 other nodes
        ep->SetVcCredits(0, 10000);
        ep->SetReceiveCallback(MakeCallback(&FullMeshTopologyTest::OnPacketReceived, this));
    }

    // Start applications (they need to be active to receive packets)
    endpoints.Start(Seconds(0));

    // Reset counters
    for (uint32_t i = 0; i < 4; ++i)
        m_receivedCount[i] = 0;

    // Send packets from node 0 to all other nodes
    Ptr<FabricEndpoint> ep0 = DynamicCast<FabricEndpoint>(endpoints.Get(0));
    uint8_t data[] = {0xAA, 0xBB, 0xCC};

    for (uint32_t dst = 1; dst < 4; ++dst)
    {
        ep0->SendData(dst, data, sizeof(data), dst, 0);
    }

    // Run simulation for enough time for packets to propagate
    Simulator::Stop(MicroSeconds(1000));
    Simulator::Run();

    // Check that each destination received its packet
    NS_TEST_EXPECT_MSG_EQ(m_receivedCount[1] >= 1, true, "Node 1 should receive packet");
    NS_TEST_EXPECT_MSG_EQ(m_receivedCount[2] >= 1, true, "Node 2 should receive packet");
    NS_TEST_EXPECT_MSG_EQ(m_receivedCount[3] >= 1, true, "Node 3 should receive packet");

    Simulator::Destroy();
}

/**
 * @brief Integration test for P2P traffic
 */
class P2pTrafficTest : public TestCase
{
  public:
    P2pTrafficTest();
    ~P2pTrafficTest() override;

    void DoRun() override;

  private:
    void TestP2pBasic();
    void TestP2pBurst();
    uint32_t m_receivedCount;
    void OnPacketReceived(uint16_t rank, Ptr<Packet> packet, FabricHeader header);
};

P2pTrafficTest::P2pTrafficTest()
    : TestCase("P2pTraffic"),
      m_receivedCount(0)
{
}

P2pTrafficTest::~P2pTrafficTest()
{
}

void
P2pTrafficTest::OnPacketReceived(uint16_t rank, Ptr<Packet> packet, FabricHeader header)
{
    m_receivedCount++;
}

void
P2pTrafficTest::DoRun()
{
    DisableUnconfiguredLinkErrors();
    TestP2pBasic();
    TestP2pBurst();
}

void
P2pTrafficTest::TestP2pBasic()
{
    // Create two nodes with P2P traffic
    GpuClusterTopologyHelper helper(2, 0);  // 2 GPUs, 0 switches
    helper.SetLinkDataRate("100Gbps");
    helper.SetLinkDelay("500ns");
    NodeContainer nodes = helper.BuildFullMesh();
    ApplicationContainer endpoints = helper.GetEndpoints();

    for (uint32_t i = 0; i < 2; ++i)
    {
        Ptr<FabricEndpoint> ep = DynamicCast<FabricEndpoint>(endpoints.Get(i));
        ep->SetNumVirtualChannels(1);
        ep->SetVcCredits(0, 1000);
    }

    // Create P2P traffic generator on node 0 sending to node 1
    Ptr<FabricEndpoint> ep0 = DynamicCast<FabricEndpoint>(endpoints.Get(0));
    Ptr<FabricEndpoint> ep1 = DynamicCast<FabricEndpoint>(endpoints.Get(1));

    Ptr<P2pTraffic> p2pTraffic = CreateObject<P2pTraffic>();
    p2pTraffic->SetEndpoint(ep0);
    p2pTraffic->SetDestRank(1);
    p2pTraffic->SetPacketSize(256);
    p2pTraffic->SetMaxPackets(10);
    p2pTraffic->SetInterval(NanoSeconds(500));

    m_receivedCount = 0;
    ep1->SetReceiveCallback(MakeCallback(&P2pTrafficTest::OnPacketReceived, this));

    p2pTraffic->Start();

    Simulator::Stop(MicroSeconds(10000));
    Simulator::Run();

    NS_TEST_EXPECT_MSG_EQ(m_receivedCount >= 10, true, "Should receive 10 packets");
    NS_TEST_EXPECT_MSG_EQ(p2pTraffic->GetPacketsSent(), 10, "Should send 10 packets");

    Simulator::Destroy();
}

void
P2pTrafficTest::TestP2pBurst()
{
    GpuClusterTopologyHelper helper(2, 0);  // 2 GPUs, 0 switches
    NodeContainer nodes = helper.BuildFullMesh();
    ApplicationContainer endpoints = helper.GetEndpoints();

    for (uint32_t i = 0; i < 2; ++i)
    {
        Ptr<FabricEndpoint> ep = DynamicCast<FabricEndpoint>(endpoints.Get(i));
        ep->SetNumVirtualChannels(1);
        ep->SetVcCredits(0, 10000);  // Large credit pool for burst
    }

    Ptr<FabricEndpoint> ep0 = DynamicCast<FabricEndpoint>(endpoints.Get(0));
    Ptr<FabricEndpoint> ep1 = DynamicCast<FabricEndpoint>(endpoints.Get(1));

    Ptr<P2pTraffic> p2pTraffic = CreateObject<P2pTraffic>();
    p2pTraffic->SetEndpoint(ep0);
    p2pTraffic->SetDestRank(1);
    p2pTraffic->SetPacketSize(512);
    p2pTraffic->SetMaxPackets(100);
    p2pTraffic->SetBurstSize(10);  // Burst mode implicitly enabled when burstSize > 1

    m_receivedCount = 0;
    ep1->SetReceiveCallback(MakeCallback(&P2pTrafficTest::OnPacketReceived, this));

    p2pTraffic->Start();

    Simulator::Stop(MicroSeconds(5000));
    Simulator::Run();

    NS_TEST_EXPECT_MSG_EQ(m_receivedCount >= 100, true, "Should receive 100 packets in burst mode");

    Simulator::Destroy();
}

/**
 * @brief Integration test for Link Degradation in topology
 */
class LinkDegradationTopologyTest : public TestCase
{
  public:
    LinkDegradationTopologyTest();
    ~LinkDegradationTopologyTest() override;

    void DoRun() override;

  private:
    void TestDegradationInSwitchedTopology();
    void TestDegradationInDirectTopology();
    uint32_t m_receivedCount;
    uint32_t m_droppedCount;
    void OnPacketReceived(uint16_t rank, Ptr<Packet> packet, FabricHeader header);
};

LinkDegradationTopologyTest::LinkDegradationTopologyTest()
    : TestCase("LinkDegradationTopology"),
      m_receivedCount(0),
      m_droppedCount(0)
{
}

LinkDegradationTopologyTest::~LinkDegradationTopologyTest()
{
}

void
LinkDegradationTopologyTest::OnPacketReceived(uint16_t rank, Ptr<Packet> packet, FabricHeader header)
{
    m_receivedCount++;
}

void
LinkDegradationTopologyTest::DoRun()
{
    DisableUnconfiguredLinkErrors();
    TestDegradationInSwitchedTopology();
    TestDegradationInDirectTopology();
}

void
LinkDegradationTopologyTest::TestDegradationInSwitchedTopology()
{
    GpuClusterTopologyHelper helper(4, 1);
    helper.SetLinkDataRate("100Gbps");
    helper.SetLinkDelay("500ns");

    // Create degradation model with packet loss
    Ptr<LinkDegradationModel> degradModel = CreateObject<LinkDegradationModel>();
    degradModel->SetBer(0.0);              // No BER (config validation requires FEC/LLR if BER>0)
    degradModel->SetPacketLossRate(0.0);  // Initially no loss
    degradModel->SetRandomStream(CreateObject<UniformRandomVariable>());

    helper.SetLinkDegradationModel(degradModel);
    NodeContainer nodes = helper.BuildFullyConnected();
    ApplicationContainer endpoints = helper.GetEndpoints();

    for (uint32_t i = 0; i < 4; ++i)
    {
        Ptr<FabricEndpoint> ep = DynamicCast<FabricEndpoint>(endpoints.Get(i));
        ep->SetNumVirtualChannels(1);
        ep->SetVcCredits(0, 10000);
    }

    // Start applications (needed for receive callbacks to be set up)
    endpoints.Start(Seconds(0));

    Ptr<FabricEndpoint> ep0 = DynamicCast<FabricEndpoint>(endpoints.Get(0));
    Ptr<FabricEndpoint> ep1 = DynamicCast<FabricEndpoint>(endpoints.Get(1));

    // Send packets without degradation
    m_receivedCount = 0;
    ep1->SetReceiveCallback(MakeCallback(&LinkDegradationTopologyTest::OnPacketReceived, this));

    for (uint32_t i = 0; i < 10; ++i)
    {
        uint8_t data[] = {0x01};
        ep0->SendData(1, data, sizeof(data), i, 0);
    }

    Simulator::Stop(MicroSeconds(1000));
    Simulator::Run();

    NS_TEST_EXPECT_MSG_EQ(m_receivedCount >= 10, true, "All packets should arrive without degradation");

    Simulator::Destroy();
}

void
LinkDegradationTopologyTest::TestDegradationInDirectTopology()
{
    // Use FullMesh topology to test degradation on direct links.
    // FullMesh provides direct links between all pairs of nodes,
    // avoiding multi-hop forwarding issues with ReorderBuffer.
    GpuClusterTopologyHelper helper(4, 0);  // 4 GPUs, 0 switches
    helper.SetLinkDataRate("100Gbps");
    helper.SetLinkDelay("500ns");

    // Create degradation model with 50% packet loss
    Ptr<LinkDegradationModel> degradModel = CreateObject<LinkDegradationModel>();
    degradModel->SetBer(0.0);              // No BER (config validation requires FEC/LLR if BER>0)
    degradModel->SetPacketLossRate(0.5);
    degradModel->SetRandomStream(CreateObject<UniformRandomVariable>());

    helper.SetLinkDegradationModel(degradModel);
    NodeContainer nodes = helper.BuildFullMesh();
    ApplicationContainer endpoints = helper.GetEndpoints();

    for (uint32_t i = 0; i < 4; ++i)
    {
        Ptr<FabricEndpoint> ep = DynamicCast<FabricEndpoint>(endpoints.Get(i));
        ep->SetNumVirtualChannels(1);
        ep->SetVcCredits(0, 10000);
        // Bypass reorder buffer for direct links - no multi-path reordering needed
        ep->SetBypassReorderBuffer(true);
    }

    // Start applications (needed for receive callbacks to be set up)
    endpoints.Start(Seconds(0));

    Ptr<FabricEndpoint> ep0 = DynamicCast<FabricEndpoint>(endpoints.Get(0));
    Ptr<FabricEndpoint> ep2 = DynamicCast<FabricEndpoint>(endpoints.Get(2));

    m_receivedCount = 0;
    ep2->SetReceiveCallback(MakeCallback(&LinkDegradationTopologyTest::OnPacketReceived, this));

    // Send many packets to test loss rate
    for (uint32_t i = 0; i < 100; ++i)
    {
        uint8_t data[] = {0x02};
        ep0->SendData(2, data, sizeof(data), i, 0);
    }

    Simulator::Stop(MicroSeconds(5000));
    Simulator::Run();

    // With 50% loss, should receive approximately 50 packets (some variation due to randomness)
    NS_TEST_EXPECT_MSG_EQ(m_receivedCount >= 20, true, "Should receive some packets despite degradation");
    NS_TEST_EXPECT_MSG_EQ(m_receivedCount <= 80, true, "Should lose some packets with degradation");

    Simulator::Destroy();
}

/**
 * @brief Integration test for Heterogeneous topology
 */
class HeterogeneousTopologyTest : public TestCase
{
  public:
    HeterogeneousTopologyTest();
    ~HeterogeneousTopologyTest() override;

    void DoRun() override;

  private:
    void TestMixedDeviceTypes();
    void TestGpuToMemoryAccess();
};

HeterogeneousTopologyTest::HeterogeneousTopologyTest()
    : TestCase("HeterogeneousTopology")
{
}

HeterogeneousTopologyTest::~HeterogeneousTopologyTest()
{
}

void
HeterogeneousTopologyTest::DoRun()
{
    DisableUnconfiguredLinkErrors();
    TestMixedDeviceTypes();
    TestGpuToMemoryAccess();
}

void
HeterogeneousTopologyTest::TestMixedDeviceTypes()
{
    GpuClusterTopologyHelper helper(4, 0);  // 4 GPUs, 0 switches
    helper.SetLinkDataRate("100Gbps");
    helper.SetLinkDelay("500ns");

    // Set different device types
    helper.SetDeviceType(0, DeviceType::GPU);
    helper.SetDeviceType(1, DeviceType::GPU);
    helper.SetDeviceType(2, DeviceType::MEMORY);
    helper.SetDeviceType(3, DeviceType::CPU);

    NodeContainer nodes = helper.BuildFullMesh();
    ApplicationContainer endpoints = helper.GetEndpoints();

    // Check device types
    Ptr<FabricEndpoint> ep0 = DynamicCast<FabricEndpoint>(endpoints.Get(0));
    Ptr<FabricEndpoint> ep1 = DynamicCast<FabricEndpoint>(endpoints.Get(1));
    Ptr<FabricEndpoint> ep2 = DynamicCast<FabricEndpoint>(endpoints.Get(2));
    Ptr<FabricEndpoint> ep3 = DynamicCast<FabricEndpoint>(endpoints.Get(3));

    NS_TEST_EXPECT_MSG_EQ(static_cast<int>(ep0->GetDeviceType()), static_cast<int>(DeviceType::GPU), "Node 0 is GPU");
    NS_TEST_EXPECT_MSG_EQ(static_cast<int>(ep1->GetDeviceType()), static_cast<int>(DeviceType::GPU), "Node 1 is GPU");
    NS_TEST_EXPECT_MSG_EQ(static_cast<int>(ep2->GetDeviceType()), static_cast<int>(DeviceType::MEMORY), "Node 2 is MEMORY");
    NS_TEST_EXPECT_MSG_EQ(static_cast<int>(ep3->GetDeviceType()), static_cast<int>(DeviceType::CPU), "Node 3 is CPU");
}

void
HeterogeneousTopologyTest::TestGpuToMemoryAccess()
{
    GpuClusterTopologyHelper helper(4, 0);  // 4 GPUs, 0 switches
    helper.SetDeviceType(0, DeviceType::GPU);
    helper.SetDeviceType(2, DeviceType::MEMORY);
    helper.SetLinkDataRate("100Gbps");
    helper.SetLinkDelay("500ns");

    NodeContainer nodes = helper.BuildFullMesh();
    ApplicationContainer endpoints = helper.GetEndpoints();

    Ptr<FabricEndpoint> memEp = DynamicCast<FabricEndpoint>(endpoints.Get(2));
    memEp->SetMemorySize(4096);
    memEp->SetMemoryLatency(NanoSeconds(100));

    for (uint32_t i = 0; i < 4; ++i)
    {
        Ptr<FabricEndpoint> ep = DynamicCast<FabricEndpoint>(endpoints.Get(i));
        ep->SetNumVirtualChannels(1);
        ep->SetVcCredits(0, 1000);
    }

    Ptr<FabricEndpoint> gpuEp = DynamicCast<FabricEndpoint>(endpoints.Get(0));

    // GPU reads from memory
    gpuEp->SendMemoryRead(2, 0, 64);

    Simulator::Stop(MicroSeconds(500));
    Simulator::Run();

    Simulator::Destroy();
}

/**
 * @brief TestSuite for GPU cluster integration tests
 */
class GpuClusterIntegrationTestSuite : public TestSuite
{
  public:
    GpuClusterIntegrationTestSuite();
};

GpuClusterIntegrationTestSuite::GpuClusterIntegrationTestSuite()
    : TestSuite("gpu-cluster-integration", Type::SYSTEM)
{
    AddTestCase(new RingTopologyTest, TestCase::Duration::QUICK);
    AddTestCase(new FullMeshTopologyTest, TestCase::Duration::QUICK);
    AddTestCase(new P2pTrafficTest, TestCase::Duration::QUICK);
    AddTestCase(new LinkDegradationTopologyTest, TestCase::Duration::QUICK);
    AddTestCase(new HeterogeneousTopologyTest, TestCase::Duration::QUICK);
}

static GpuClusterIntegrationTestSuite g_gpuClusterIntegrationTestSuite;
