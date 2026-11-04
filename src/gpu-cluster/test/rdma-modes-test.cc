/* SPDX-License-Identifier: GPL-2.0-only */
#include "ns3/double.h"
#include "ns3/fabric-endpoint.h"
#include "ns3/node-container.h"
#include "ns3/point-to-point-helper.h"
#include "ns3/rdma-submission-model.h"
#include "ns3/roce-payload-builder.h"
#include "ns3/roce-protocol-model.h"
#include "ns3/simulator.h"
#include "ns3/string.h"
#include "ns3/test.h"
#include "ns3/uinteger.h"

using namespace ns3;

class RdmaSharedHostTest : public TestCase
{
  public:
    RdmaSharedHostTest()
        : TestCase("RDMA host DRAM contention shared across endpoints")
    {
    }

    void DoRun() override
    {
        auto host = std::make_shared<RdmaHostMemoryResources>();
        RdmaConfig c;
        c.initiator = "cpu";
        c.dataPath = "host";
        c.hostReadGbps = 8;
        auto a = CreateObject<RdmaSubmissionModel>();
        a->Configure(c);
        a->SetHostMemoryResources(host);
        auto b = CreateObject<RdmaSubmissionModel>();
        b->Configure(c);
        b->SetHostMemoryResources(host);
        NS_TEST_ASSERT_MSG_EQ(a->ReserveSourceRead(100).GetNanoSeconds(), 100, "First host read");
        NS_TEST_ASSERT_MSG_EQ(b->ReserveSourceRead(100).GetNanoSeconds(),
                              200,
                              "Second NIC shares host bus");
        NS_TEST_ASSERT_MSG_EQ(a->GetStats().hbmReadBytes, 0, "Host payload bypasses HBM");
        a->Dispose();
        b->Dispose();
        Simulator::Destroy();
    }
};

class RdmaStagingQuietTest : public TestCase
{
  public:
    RdmaStagingQuietTest()
        : TestCase("RDMA quiet covers staged chunks before WR publication")
    {
    }

    void DoRun() override
    {
        RdmaConfig c;
        c.initiator = "cpu";
        c.dataPath = "staged";
        c.stagingChunkBytes = 512;
        c.stagingBufferBytes = 512;
        c.d2hLatency = NanoSeconds(20);
        auto model = CreateObject<RdmaSubmissionModel>();
        model->Configure(c);
        uint32_t executions = 0, completions = 0;
        model->Submit(
            1,
            1536,
            [model, &executions](uint64_t id) {
                ++executions;
                model->NotifyFirstWire(id);
                Simulator::Schedule(NanoSeconds(100), [model, id] {
                    model->NotifyPacketAck(id, model->GetBytesForWork(id));
                });
            },
            0,
            [&](bool ok) {
                if (ok)
                {
                    ++completions;
                }
            });
        bool quiet = false;
        model->Quiet([&](bool ok) { quiet = ok; });
        NS_TEST_ASSERT_MSG_EQ(quiet, false, "D2H-pending requests belong to quiet watermark");
        Simulator::Run();
        NS_TEST_ASSERT_MSG_EQ(quiet, true, "All staging chunks drain");
        NS_TEST_ASSERT_MSG_EQ(executions, 3, "One WR per staging chunk");
        NS_TEST_ASSERT_MSG_EQ(completions, 1, "One callback per user request");
        NS_TEST_ASSERT_MSG_EQ(Simulator::Now().GetNanoSeconds(),
                              360,
                              "One staging slot backpressures copies");
        NS_TEST_ASSERT_MSG_EQ(model->GetStats().sourceD2hBytes, 1536, "Exact D2H byte accounting");
        NS_TEST_ASSERT_MSG_EQ(model->GetStats().requestStartupSamples,
                              1,
                              "One startup sample per user request");
        model->Dispose();
        Simulator::Destroy();
    }
};

class RdmaReadinessTest : public TestCase
{
  public:
    RdmaReadinessTest()
        : TestCase("RDMA host completion precedes staged GPU data readiness")
    {
    }

    void DoRun() override
    {
        NodeContainer nodes;
        nodes.Create(2);
        PointToPointHelper link;
        link.SetDeviceAttribute("DataRate", StringValue("200Gbps"));
        link.SetDeviceAttribute("Mtu", UintegerValue(65535));
        link.SetChannelAttribute("Delay", TimeValue(Time(0)));
        auto devices = link.Install(nodes);
        Ptr<FabricEndpoint> endpoints[2];
        RdmaConfig config;
        config.initiator = "cpu";
        config.dataPath = "staged";
        config.stagingChunkBytes = 16;
        config.stagingBufferBytes = 32;
        config.d2hLatency = NanoSeconds(100);
        config.h2dLatency = NanoSeconds(1000);
        for (uint32_t i = 0; i < 2; ++i)
        {
            auto ep = CreateObject<FabricEndpoint>();
            endpoints[i] = ep;
            ep->SetRank(i);
            ep->SetLocalRankRange(i, 1);
            ep->SetFabricType(FabricType::ROCE);
            auto protocol = CreateObject<RoceProtocolModel>();
            protocol->SetAttribute("WireEfficiency", DoubleValue(1));
            ep->SetProtocolModel(protocol);
            ep->SetProtocolPayloadBuilder(CreateObject<RocePayloadBuilder>());
            ep->SetBypassReorderBuffer(true);
            ep->SetLaunchDelay(0);
            ep->AddNetDevice(devices.Get(i));
            ep->SetRoutingEntry(1 - i, 0);
            ep->SetNeighborMac(1 - i, Mac48Address::ConvertFrom(devices.Get(1 - i)->GetAddress()));
            auto runtime = CreateObject<RdmaSubmissionModel>();
            runtime->Configure(config);
            ep->SetRdmaSubmissionModel(runtime);
            nodes.Get(i)->AddApplication(ep);
            ep->SetStartTime(Seconds(0));
            ep->Initialize();
        }
        Time completed, ready;
        bool done = false;
        endpoints[1]->SetReceiveCallback(MakeCallback(&RdmaReadinessTest::Received, this));
        m_ready = &ready;
        endpoints[0]->SubmitRdmaWrite(1, 16, 1, 1, 0, 0, [&](bool ok) {
            done = ok;
            completed = Simulator::Now();
        });
        Simulator::Stop(MicroSeconds(10));
        Simulator::Run();
        NS_TEST_ASSERT_MSG_EQ(done, true, "Public RDMA completion callback executes");
        NS_TEST_ASSERT_MSG_EQ(ready > completed,
                              true,
                              "Host RDMA completion does not imply GPU readiness");
        NS_TEST_ASSERT_MSG_EQ(endpoints[0]->GetRdmaSubmissionModel()->GetStats().completedRequests,
                              1,
                              "One request completes");
        NS_TEST_ASSERT_MSG_EQ(
            endpoints[1]->GetRdmaSubmissionModel()->GetStats().destinationH2dBytes,
            16,
            "Target H2D modeled");
        for (auto ep : endpoints)
        {
            ep->Dispose();
        }
        Simulator::Destroy();
    }

    void Received(uint16_t, Ptr<Packet>, FabricHeader)
    {
        *m_ready = Simulator::Now();
        Simulator::Stop();
    }

  private:
    Time* m_ready{nullptr};
};

class RdmaModesSuite : public TestSuite
{
  public:
    RdmaModesSuite()
        : TestSuite("gpu-cluster-rdma-modes", Type::UNIT)
    {
        AddTestCase(new RdmaSharedHostTest, TestCase::Duration::QUICK);
        AddTestCase(new RdmaStagingQuietTest, TestCase::Duration::QUICK);
        AddTestCase(new RdmaReadinessTest, TestCase::Duration::QUICK);
    }
} g_rdmaModesSuite;
