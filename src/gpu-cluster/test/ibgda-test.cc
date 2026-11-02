/* SPDX-License-Identifier: GPL-2.0-only */
#include "ns3/ibgda-submission-model.h"
#include "ns3/fabric-endpoint.h"
#include "ns3/roce-protocol-model.h"
#include "ns3/roce-payload-builder.h"
#include "ns3/point-to-point-helper.h"
#include "ns3/node-container.h"
#include "ns3/double.h"
#include "ns3/uinteger.h"
#include "ns3/simulator.h"
#include "ns3/test.h"
#include <set>

using namespace ns3;

class IbgdaPipelineTest : public TestCase
{
  public:
    IbgdaPipelineTest() : TestCase("IBGDA phase timing, source DMA and quiet completion") {}
    void DoRun() override
    {
        auto model=CreateObject<IbgdaSubmissionModel>();
        IbgdaConfig c;
        c.wqeBuild=NanoSeconds(100); c.publish=NanoSeconds(20);
        c.doorbell=NanoSeconds(30); c.wqeFetch=NanoSeconds(40);
        c.dmaReadLatency=NanoSeconds(50); c.cqWrite=NanoSeconds(30);
        c.gpuPoll=NanoSeconds(20);
        model->Configure(c);
        IbgdaWorkTiming timing;
        model->SetObserver([&](const IbgdaWorkTiming& t){timing=t;});
        model->Submit(1,64,[model](uint64_t id){
            Time delay=model->ReserveSourceRead(64);
            Simulator::Schedule(delay,[model,id]{
                model->NotifyFirstWire(id);
                Simulator::Schedule(NanoSeconds(300),[model,id]{model->NotifyPacketAck(id,64);});
            });
        });
        bool done=false;
        model->Quiet([&](bool ok){done=ok;});
        Simulator::Run();
        NS_TEST_ASSERT_MSG_EQ(done,true,"Quiet must observe CQ completion");
        NS_TEST_ASSERT_MSG_EQ(timing.published.GetNanoSeconds(),120,"WQE publication");
        NS_TEST_ASSERT_MSG_EQ(timing.execute.GetNanoSeconds(),190,"Doorbell and fetch");
        NS_TEST_ASSERT_MSG_EQ(timing.firstWire.GetNanoSeconds(),240,"DMA precedes first wire");
        NS_TEST_ASSERT_MSG_EQ(timing.acknowledged.GetNanoSeconds(),540,"Feedback precedes CQ");
        NS_TEST_ASSERT_MSG_EQ(timing.observed.GetNanoSeconds(),590,"GPU observes CQ write");
        NS_TEST_ASSERT_MSG_EQ(model->GetStats().startupTotalNs,240,"Startup is measured");
        model->Dispose(); Simulator::Destroy();
    }
};

class IbgdaQueueTest : public TestCase
{
  public:
    IbgdaQueueTest() : TestCase("IBGDA finite WR depth, batching, short CQ tail and parallel QPs") {}
    void DoRun() override
    {
        auto model=CreateObject<IbgdaSubmissionModel>();
        IbgdaConfig c;
        c.producers=4; c.qpsPerPeer=2; c.queueDepth=3;
        c.doorbellBatch=3; c.cqModeration=3;
        c.wqeBuild=NanoSeconds(10); c.batchTimeout=NanoSeconds(40);
        c.cqWrite=NanoSeconds(10); c.gpuPoll=NanoSeconds(10);
        model->Configure(c);
        std::set<uint64_t> executed;
        for (uint32_t i=0;i<17;++i)
        {
            model->Submit(1,9000,[model,&executed](uint64_t id){
                executed.insert(id);
                model->NotifyFirstWire(id);
                // Deliberately ACK three fragments in reverse byte sizes.
                Simulator::Schedule(NanoSeconds(100),[model,id]{model->NotifyPacketAck(id,808);});
                Simulator::Schedule(NanoSeconds(110),[model,id]{model->NotifyPacketAck(id,4096);});
                Simulator::Schedule(NanoSeconds(120),[model,id]{model->NotifyPacketAck(id,4096);});
            });
        }
        bool done=false; model->Quiet([&](bool ok){done=ok;});
        Simulator::Run();
        NS_TEST_ASSERT_MSG_EQ(done,true,"Partial doorbell / CQ tails must drain");
        NS_TEST_ASSERT_MSG_EQ(executed.size(),17,"Every WR executes exactly once");
        NS_TEST_ASSERT_MSG_EQ(model->GetStats().completed,17,"All fragments required");
        NS_TEST_ASSERT_MSG_EQ(model->GetOutstanding(),0,"All queue slots recycled");
        NS_TEST_ASSERT_MSG_EQ(model->GetStats().peakQpOutstanding,3,"WR depth per QP");
        NS_TEST_ASSERT_MSG_EQ(model->GetStats().peakOutstanding,6,"Independent QPs");
        NS_TEST_ASSERT_MSG_LT(model->GetStats().doorbells,17,"Batching amortizes doorbells");
        model->Dispose(); Simulator::Destroy();
    }
};

class IbgdaFenceTest : public TestCase
{
  public:
    IbgdaFenceTest() : TestCase("IBGDA cross-QP same-peer fence and quiet watermark") {}
    void DoRun() override
    {
        auto model=CreateObject<IbgdaSubmissionModel>();
        IbgdaConfig c; c.producers=2; c.qpsPerPeer=2;
        model->Configure(c);
        Time firstAck,secondStart;
        model->Submit(1,8,[model,&firstAck](uint64_t id){
            Simulator::Schedule(NanoSeconds(100),[model,id,&firstAck]{
                firstAck=Simulator::Now(); model->NotifyPacketAck(id,8);
            });
        },0);
        model->Fence(1);
        model->Submit(1,8,[model,&secondStart](uint64_t id){
            secondStart=Simulator::Now(); model->NotifyPacketAck(id,8);
        },1);
        bool done=false;model->Quiet([&](bool ok){done=ok;});
        Simulator::Run();
        NS_TEST_ASSERT_MSG_EQ(done,true,"Fence must make forward progress");
        NS_TEST_ASSERT_MSG_EQ(secondStart >= firstAck,true,"Same-peer ordering across QPs");
        NS_TEST_ASSERT_MSG_EQ(model->GetStats().completed,2,"Both WRs complete");
        model->Dispose(); Simulator::Destroy();
    }
};

class IbgdaWatermarkTest : public TestCase
{
  public:
    IbgdaWatermarkTest() : TestCase("IBGDA quiet watermark and per-peer round-robin queues") {}
    void DoRun() override
    {
        auto model=CreateObject<IbgdaSubmissionModel>();
        IbgdaConfig c;c.qpsPerPeer=2;c.mapQpByProducer=false;
        model->Configure(c);
        Time quietTime;
        model->Submit(1,8,[model](uint64_t id){
            Simulator::Schedule(NanoSeconds(10),[model,id]{model->NotifyPacketAck(id,8);});
        });
        model->Quiet([&](bool ok){if (ok)quietTime=Simulator::Now();});
        model->Submit(2,8,[model](uint64_t id){
            Simulator::Schedule(NanoSeconds(100),[model,id]{model->NotifyPacketAck(id,8);});
        });
        std::map<uint16_t,std::set<uint32_t>> queues;
        model->SetObserver([&](const IbgdaWorkTiming& t){queues[t.peer].insert(t.qp);});
        for (uint32_t i=0;i<6;++i)
            model->Submit(1+i%2,8,[model](uint64_t id){model->NotifyPacketAck(id,8);});
        Simulator::Run();
        NS_TEST_ASSERT_MSG_EQ(quietTime.GetNanoSeconds(),10,"Quiet excludes later requests");
        NS_TEST_ASSERT_MSG_EQ(queues[1].size(),2,"Round robin is independent per peer");
        NS_TEST_ASSERT_MSG_EQ(queues[2].size(),2,"Both peers use both QPs");
        model->Dispose();Simulator::Destroy();
    }
};

class IbgdaFailureTest : public TestCase
{
  public:
    IbgdaFailureTest() : TestCase("IBGDA permanent loss cannot become a successful completion") {}
    void DoRun() override
    {
        auto model=CreateObject<IbgdaSubmissionModel>();
        model->Configure(IbgdaConfig{});
        model->Submit(1,8,[model](uint64_t id){
            model->NotifyFailure(id);model->NotifyFailure(id);
        });
        bool done=false;model->Quiet([&](bool){done=true;});
        Simulator::Run();
        NS_TEST_ASSERT_MSG_EQ(done,false,"A failed WR cannot satisfy quiet");
        NS_TEST_ASSERT_MSG_EQ(model->GetStats().failed,1,"Count each failed WR once");
        NS_TEST_ASSERT_MSG_EQ(model->GetStats().completed,0,"No success for failed work");
        model->Dispose();Simulator::Destroy();
    }
};

class IbgdaRepeatedFenceTest : public TestCase
{
  public:
    IbgdaRepeatedFenceTest() : TestCase("IBGDA repeated fences retain each request's ordering watermark") {}
    void DoRun() override
    {
        auto model=CreateObject<IbgdaSubmissionModel>();
        IbgdaConfig c;c.producers=3;c.qpsPerPeer=3;model->Configure(c);
        std::vector<Time> execution(3),ack(3);
        for (uint32_t i=0;i<3;++i)
        {
            model->Submit(1,8,[model,&execution,&ack,i](uint64_t id){
                execution[i]=Simulator::Now();
                Simulator::Schedule(NanoSeconds(100),[model,&ack,id,i]{
                    ack[i]=Simulator::Now();model->NotifyPacketAck(id,8);
                });
            },i);
            model->Fence(1);
        }
        Simulator::Run();
        NS_TEST_ASSERT_MSG_EQ(execution[1] >= ack[0],true,"Second WR honors the first fence");
        NS_TEST_ASSERT_MSG_EQ(execution[2] >= ack[1],true,"Third WR honors the second fence");
        NS_TEST_ASSERT_MSG_EQ(model->GetStats().completed,3,"Repeated fences drain");
        model->Dispose();Simulator::Destroy();
    }
};

class IbgdaDisposeTest : public TestCase
{
  public:
    IbgdaDisposeTest() : TestCase("IBGDA disposal cancels unpublished work and batch timers") {}
    void DoRun() override
    {
        auto model=CreateObject<IbgdaSubmissionModel>();
        IbgdaConfig c; c.wqeBuild=NanoSeconds(100);
        c.doorbellBatch=4; c.batchTimeout=NanoSeconds(1000);
        model->Configure(c);
        bool executed=false;
        model->Submit(1,8,[&](uint64_t){executed=true;});
        Simulator::Schedule(NanoSeconds(50),[model]{model->Dispose();});
        Simulator::Run();
        NS_TEST_ASSERT_MSG_EQ(executed,false,"No deferred callbacks after disposal");
        Simulator::Destroy();
    }
};

class IbgdaWireTest : public TestCase
{
  public:
    IbgdaWireTest() : TestCase("IBGDA actual endpoint ACK, MTU fragments, DMA and physical first wire") {}
    void DoRun() override
    {
        NodeContainer nodes;nodes.Create(2);
        PointToPointHelper helper;
        helper.SetDeviceAttribute("DataRate",StringValue("200Gbps"));
        helper.SetDeviceAttribute("Mtu",UintegerValue(65535));
        helper.SetChannelAttribute("Delay",TimeValue(NanoSeconds(649)));
        auto devices=helper.Install(nodes);
        Ptr<FabricEndpoint> endpoints[2];
        IbgdaConfig config;
        config.queueDepth=2;config.cqModeration=2;
        config.wqeBuild=NanoSeconds(100);config.doorbell=NanoSeconds(100);
        config.wqeFetch=NanoSeconds(100);config.dmaReadLatency=NanoSeconds(50);
        config.dmaWriteLatency=NanoSeconds(50);config.cqWrite=NanoSeconds(50);
        config.gpuPoll=NanoSeconds(50);
        for (uint32_t i=0;i<2;++i)
        {
            auto ep=CreateObject<FabricEndpoint>();endpoints[i]=ep;
            ep->SetRank(i);ep->SetLocalRankRange(i,1);
            ep->SetFabricType(FabricType::ROCE);
            auto protocol=CreateObject<RoceProtocolModel>();
            protocol->SetAttribute("WireEfficiency",DoubleValue(1));
            ep->SetProtocolModel(protocol);
            ep->SetProtocolPayloadBuilder(CreateObject<RocePayloadBuilder>());
            ep->SetBypassReorderBuffer(true);ep->SetLaunchDelay(0);
            ep->AddNetDevice(devices.Get(i));
            ep->SetRoutingEntry(1-i,0);
            ep->SetNeighborMac(1-i,Mac48Address::ConvertFrom(devices.Get(1-i)->GetAddress()));
            auto model=CreateObject<IbgdaSubmissionModel>();model->Configure(config);
            ep->SetIbgdaSubmissionModel(model);
            nodes.Get(i)->AddApplication(ep);ep->SetStartTime(Seconds(0));ep->Initialize();
        }
        auto sender=endpoints[0]->GetIbgdaSubmissionModel();
        for (uint32_t i=0;i<5;++i) endpoints[0]->SendBulkWireTransferSize(1,9000,1,1,0);
        bool done=false;sender->Quiet([&](bool ok){done=ok;});
        Simulator::Stop(MilliSeconds(1));Simulator::Run();
        NS_TEST_ASSERT_MSG_EQ(done,true,"WRs must complete using actual wire ACKs");
        NS_TEST_ASSERT_MSG_EQ(sender->GetStats().completed,5,"Short CQ tail completes");
        NS_TEST_ASSERT_MSG_EQ(sender->GetStats().peakQpOutstanding,2,"WR depth independent of fragments");
        NS_TEST_ASSERT_MSG_EQ(sender->GetStats().startupSamples,5,"Physical first-wire trace for every WR");
        NS_TEST_ASSERT_MSG_EQ(sender->GetStats().startupTotalNs >= 350*5,true,"WQE and DMA costs included");
        NS_TEST_ASSERT_MSG_EQ(endpoints[1]->GetRxPackets(),15,"9000 B splits into three packets per WR");
        for (auto ep : endpoints) ep->Dispose();
        Simulator::Destroy();
    }
};

class IbgdaSuite : public TestSuite
{
  public:
    IbgdaSuite() : TestSuite("gpu-cluster-ibgda",Type::UNIT)
    {
        AddTestCase(new IbgdaPipelineTest,TestCase::Duration::QUICK);
        AddTestCase(new IbgdaQueueTest,TestCase::Duration::QUICK);
        AddTestCase(new IbgdaFenceTest,TestCase::Duration::QUICK);
        AddTestCase(new IbgdaWatermarkTest,TestCase::Duration::QUICK);
        AddTestCase(new IbgdaFailureTest,TestCase::Duration::QUICK);
        AddTestCase(new IbgdaRepeatedFenceTest,TestCase::Duration::QUICK);
        AddTestCase(new IbgdaDisposeTest,TestCase::Duration::QUICK);
        AddTestCase(new IbgdaWireTest,TestCase::Duration::QUICK);
    }
} g_ibgdaSuite;
