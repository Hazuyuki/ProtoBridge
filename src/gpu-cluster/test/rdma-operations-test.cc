/* SPDX-License-Identifier: GPL-2.0-only */
#include "ns3/double.h"
#include "ns3/fabric-endpoint.h"
#include "ns3/node-container.h"
#include "ns3/point-to-point-helper.h"
#include "ns3/roce-payload-builder.h"
#include "ns3/roce-protocol-model.h"
#include "ns3/simulator.h"
#include "ns3/string.h"
#include "ns3/test.h"
#include "ns3/uinteger.h"

#include <set>

using namespace ns3;

namespace
{
struct Pair
{
    NodeContainer nodes;
    Ptr<FabricEndpoint> endpoint[2];

    Pair(RdmaConfig config)
    {
        nodes.Create(2);
        PointToPointHelper link;
        link.SetDeviceAttribute("DataRate", StringValue("200Gbps"));
        link.SetDeviceAttribute("Mtu", UintegerValue(65535));
        link.SetChannelAttribute("Delay", TimeValue(NanoSeconds(20)));
        auto devices = link.Install(nodes);
        for (uint32_t i = 0; i < 2; ++i)
        {
            auto ep = CreateObject<FabricEndpoint>();
            endpoint[i] = ep;
            ep->SetRank(i);
            ep->SetNodeId(i);
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
            auto model = CreateObject<RdmaSubmissionModel>();
            model->Configure(config);
            ep->SetRdmaSubmissionModel(model);
            ep->RegisterRdmaMemory(0, 1 << 20);
            nodes.Get(i)->AddApplication(ep);
            ep->SetStartTime(Seconds(0));
            ep->Initialize();
        }
    }

    ~Pair()
    {
        for (auto ep : endpoint)
        {
            ep->Dispose();
        }
        Simulator::Destroy();
    }

    Ptr<RdmaOperationEngine> Engine(uint32_t i)
    {
        return endpoint[i]->GetRdmaOperationEngine();
    }

    Ptr<RdmaSubmissionModel> Model(uint32_t i)
    {
        return endpoint[i]->GetRdmaSubmissionModel();
    }

    void Run()
    {
        Simulator::Stop(MilliSeconds(10));
        Simulator::Run();
    }
};

RdmaConfig
OperationConfig(const std::string& actor, const std::string& path)
{
    RdmaConfig config;
    config.initiator = actor;
    config.dataPath = path;
    config.mtuBytes = 256;
    config.stagingChunkBytes = 1024;
    config.stagingBufferBytes = 2048;
    config.d2hLatency = NanoSeconds(100);
    config.h2dLatency = NanoSeconds(1000);
    return config;
}
} // namespace

class RdmaReadOperationTest : public TestCase
{
    std::string m_actor, m_path;

  public:
    RdmaReadOperationTest(std::string actor, std::string path)
        : TestCase("RDMA Read returns bytes: " + actor + " " + path),
          m_actor(actor),
          m_path(path)
    {
    }

    void DoRun() override
    {
        Pair pair(OperationConfig(m_actor, m_path));
        std::vector<uint8_t> data(3017);
        for (uint32_t i = 0; i < data.size(); ++i)
        {
            data[i] = (i * 13 + 7) % 251;
        }
        pair.Engine(1)->WriteMemory(4096, data);
        RdmaRequest request;
        request.opcode = RdmaOpcode::READ;
        request.peer = 1;
        request.bytes = data.size();
        request.remoteAddress = 4096;
        request.localAddress = 8192;
        bool done = false;
        pair.endpoint[0]->SubmitRdmaRead(request, [&](const RdmaCompletion& result) {
            done = result.success;
        });
        pair.Run();
        NS_TEST_ASSERT_MSG_EQ(done, true, "Read completes after response data");
        NS_TEST_ASSERT_MSG_EQ(pair.Engine(0)->ReadMemory(8192, data.size()) == data,
                              true,
                              "Exact read payload");
        NS_TEST_ASSERT_MSG_EQ(pair.Model(0)->GetStats().sourceD2hBytes,
                              0,
                              "Read request carries no payload D2H");
        NS_TEST_ASSERT_MSG_EQ(pair.Model(1)->GetStats().sourceD2hBytes,
                              m_path == "staged" ? data.size() : 0,
                              "Responder D2H");
        NS_TEST_ASSERT_MSG_EQ(pair.Model(0)->GetStats().destinationH2dBytes,
                              m_path == "staged" ? data.size() : 0,
                              "Requester H2D");
        NS_TEST_ASSERT_MSG_EQ(pair.Model(0)->GetStats().completedRequests, 1, "One logical GET");
    }
};

class RdmaSendOperationTest : public TestCase
{
    RdmaOpcode m_opcode;

  public:
    explicit RdmaSendOperationTest(RdmaOpcode opcode)
        : TestCase("RDMA receive matching and immediate: " + RdmaOpcodeName(opcode)),
          m_opcode(opcode)
    {
    }

    void DoRun() override
    {
        auto config = OperationConfig("cpu", "staged");
        config.rnrRetry = NanoSeconds(1000);
        Pair pair(config);
        std::vector<uint8_t> data(731, 0x5a);
        pair.Engine(0)->WriteMemory(0, data);
        RdmaRequest request;
        request.opcode = m_opcode;
        request.peer = 1;
        request.bytes = data.size();
        request.remoteAddress = 8192;
        request.immediate = 0x1234abcd;
        bool done = false, received = false;
        Simulator::Schedule(NanoSeconds(140), [&] {
            pair.endpoint[1]->PostRdmaReceive(
                77,
                4096,
                m_opcode == RdmaOpcode::WRITE_IMM ? 0 : 1024,
                [&](const RdmaReceiveCompletion& completion) {
                    received = true;
                    NS_TEST_ASSERT_MSG_EQ(completion.receiveId, 77, "Matched posted RQ entry");
                    NS_TEST_ASSERT_MSG_EQ(completion.bytes, data.size(), "Receive byte length");
                    NS_TEST_ASSERT_MSG_EQ(completion.hasImmediate,
                                          m_opcode != RdmaOpcode::SEND,
                                          "Immediate flag");
                    if (completion.hasImmediate)
                    {
                        NS_TEST_ASSERT_MSG_EQ(completion.immediate, 0x1234abcd, "Immediate value");
                    }
                },
                0);
        });
        pair.endpoint[0]->SubmitRdmaOperation(request, [&](const RdmaCompletion& completion) {
            done = completion.success;
        });
        pair.Run();
        NS_TEST_ASSERT_MSG_EQ(done && received, true, "Source and receive CQ complete");
        uint64_t address = m_opcode == RdmaOpcode::WRITE_IMM ? 8192 : 4096;
        NS_TEST_ASSERT_MSG_EQ(pair.Engine(1)->ReadMemory(address, data.size()) == data,
                              true,
                              "Receive address semantics");
        NS_TEST_ASSERT_MSG_GT(pair.Engine(0)->GetStats().rnrRetries,
                              0,
                              "No posted receive triggers retry");
        NS_TEST_ASSERT_MSG_EQ(pair.Engine(1)->GetStats().receiveCompletions,
                              1,
                              "Exactly one receive completion");
    }
};

class RdmaAtomicOperationTest : public TestCase
{
    std::string m_path;

  public:
    explicit RdmaAtomicOperationTest(std::string path)
        : TestCase("RDMA atomic values and resource limits: " + path),
          m_path(path)
    {
    }

    void DoRun() override
    {
        auto config = OperationConfig("gpu", m_path);
        config.qpsPerPeer = 4;
        config.mapQpByProducer = false;
        config.maxReadAtomic = 2;
        config.responderDepth = 1;
        Pair pair(config);
        std::set<uint64_t> previous;
        uint32_t done = 0;
        for (uint32_t i = 0; i < 32; ++i)
        {
            RdmaRequest request;
            request.peer = 1;
            request.localAddress = 4096 + i * 8;
            request.remoteAddress = 0;
            request.operand = 1;
            pair.endpoint[0]->SubmitRdmaFetchAdd(request, [&](const RdmaCompletion& completion) {
                NS_TEST_ASSERT_MSG_EQ(completion.success, true, "Fetch-add succeeds");
                previous.insert(completion.value);
                ++done;
            });
        }
        pair.Run();
        NS_TEST_ASSERT_MSG_EQ(done, 32, "All atomic responses return");
        NS_TEST_ASSERT_MSG_EQ(previous.size(), 32, "Each update returns a distinct old value");
        NS_TEST_ASSERT_MSG_EQ(*previous.begin(), 0, "First old value");
        NS_TEST_ASSERT_MSG_EQ(*previous.rbegin(), 31, "Last old value");
        NS_TEST_ASSERT_MSG_EQ(pair.Engine(1)->ReadWord(0), 32, "Remote memory updated atomically");
        NS_TEST_ASSERT_MSG_LT_OR_EQ(pair.Model(0)->GetStats().peakReadAtomic,
                                    2,
                                    "Per-QP initiator credits");
        NS_TEST_ASSERT_MSG_EQ(pair.Engine(1)->GetStats().responsePeak,
                              1,
                              "Finite responder resource");
        RdmaRequest cas;
        cas.peer = 1;
        cas.remoteAddress = 0;
        cas.localAddress = 16384;
        cas.compare = 32;
        cas.operand = 99;
        uint64_t result = 0;
        pair.endpoint[0]->SubmitRdmaCompareSwap(cas,
                                                [&](const RdmaCompletion& c) { result = c.value; });
        pair.Run();
        NS_TEST_ASSERT_MSG_EQ(result, 32, "CAS returns previous value on match");
        NS_TEST_ASSERT_MSG_EQ(pair.Engine(1)->ReadWord(0), 99, "CAS match swaps");
        pair.endpoint[0]->SubmitRdmaCompareSwap(cas,
                                                [&](const RdmaCompletion& c) { result = c.value; });
        pair.Run();
        NS_TEST_ASSERT_MSG_EQ(result, 99, "CAS miss still returns old value");
        NS_TEST_ASSERT_MSG_EQ(pair.Engine(1)->ReadWord(0), 99, "CAS miss preserves memory");
    }
};

class RdmaSignalOperationTest : public TestCase
{
    std::string m_path;
    uint32_t m_qps;

  public:
    explicit RdmaSignalOperationTest(std::string path, uint32_t qps = 4)
        : TestCase("RDMA put-signal data ordering and quiet: " + path +
                   " qps=" + std::to_string(qps)),
          m_path(path),
          m_qps(qps)
    {
    }

    void DoRun() override
    {
        auto config = OperationConfig("gpu", m_path);
        config.qpsPerPeer = m_qps;
        config.mapQpByProducer = false;
        Pair pair(config);
        std::vector<uint8_t> data(3017, 0x6d);
        pair.Engine(0)->WriteMemory(0, data);
        RdmaRequest request;
        request.peer = 1;
        request.bytes = data.size();
        request.remoteAddress = 8192;
        request.signalAddress = 65536;
        request.signalValue = 7;
        bool signaled = false, done = false, quiet = false;
        pair.Engine(1)->WaitSignal(65536, RdmaComparison::EQ, 7, [&](uint64_t value) {
            signaled = true;
            NS_TEST_ASSERT_MSG_EQ(pair.Engine(1)->ReadMemory(8192, data.size()) == data,
                                  true,
                                  "Signal follows all payload bytes");
            NS_TEST_ASSERT_MSG_EQ(value, 7, "Signal value");
        });
        pair.endpoint[0]->SubmitRdmaPutSignal(request,
                                              [&](const RdmaCompletion& c) { done = c.success; });
        pair.Model(0)->Quiet([&](bool ok) {
            quiet = ok;
            NS_TEST_ASSERT_MSG_EQ(signaled, true, "Quiet includes signal child");
        });
        pair.Run();
        NS_TEST_ASSERT_MSG_EQ(done && quiet && signaled, true, "Logical put and wait complete");
        NS_TEST_ASSERT_MSG_EQ(pair.Model(0)->GetStats().completedRequests,
                              1,
                              "Signal belongs to parent request");
        NS_TEST_ASSERT_MSG_EQ(pair.Model(0)->GetStats().completed,
                              m_path == "staged" ? 4 : 2,
                              "Data WR(s) plus signal WR");
        NS_TEST_ASSERT_MSG_EQ(pair.Engine(1)->GetStats().signalUpdates, 1, "One signal update");
        NS_TEST_ASSERT_MSG_EQ(pair.Engine(1)->TestSignal(65536, RdmaComparison::GE, 7),
                              true,
                              "Local signal test");
    }
};

class RdmaOrderedReadTest : public TestCase
{
    std::string m_path;

  public:
    explicit RdmaOrderedReadTest(std::string path)
        : TestCase("RC Write before Read visibility: " + path),
          m_path(path)
    {
    }

    void DoRun() override
    {
        auto config = OperationConfig("cpu", m_path);
        config.dmaReadLatency = NanoSeconds(100);
        config.dmaWriteLatency = NanoSeconds(100);
        Pair pair(config);
        std::vector<uint8_t> data(3017, 0x71);
        pair.Engine(0)->WriteMemory(0, data);
        RdmaRequest write;
        write.peer = 1;
        write.bytes = data.size();
        write.remoteAddress = 8192;
        RdmaRequest read = write;
        read.opcode = RdmaOpcode::READ;
        read.localAddress = 16384;
        bool complete = false;
        pair.endpoint[0]->SubmitRdmaOperation(write);
        pair.endpoint[0]->SubmitRdmaRead(read,
                                         [&](const RdmaCompletion& c) { complete = c.success; });
        pair.Run();
        NS_TEST_ASSERT_MSG_EQ(complete, true, "Read behind pending write drains");
        NS_TEST_ASSERT_MSG_EQ(pair.Engine(0)->ReadMemory(16384, data.size()) == data,
                              true,
                              "Read cannot overtake source/destination DMA");
    }
};

class RdmaFailureOperationTest : public TestCase
{
  public:
    RdmaFailureOperationTest()
        : TestCase("RDMA RNR exhaustion and invalid remote key report failure")
    {
    }

    void DoRun() override
    {
        auto config = OperationConfig("cpu", "host");
        config.rnrRetryLimit = 2;
        Pair pair(config);
        RdmaRequest request;
        request.opcode = RdmaOpcode::SEND;
        request.peer = 1;
        request.bytes = 64;
        bool called = false, success = true, quiet = true;
        pair.endpoint[0]->SubmitRdmaOperation(request, [&](const RdmaCompletion& c) {
            called = true;
            success = c.success;
        });
        pair.Model(0)->Quiet([&](bool ok) { quiet = ok; });
        pair.Run();
        NS_TEST_ASSERT_MSG_EQ(called && !success && !quiet,
                              true,
                              "RNR exhaustion returns an error completion");
        NS_TEST_ASSERT_MSG_EQ(pair.Engine(0)->GetStats().rnrRetries, 2, "Configured retry budget");
        NS_TEST_ASSERT_MSG_EQ(pair.Model(0)->GetOutstanding(),
                              0,
                              "Failed WR releases its queue slot");
        called = false;
        success = true;
        request.opcode = RdmaOpcode::READ;
        request.remoteKey = 99;
        pair.endpoint[0]->SubmitRdmaOperation(request, [&](const RdmaCompletion& c) {
            called = true;
            success = c.success;
        });
        pair.Run();
        NS_TEST_ASSERT_MSG_EQ(called && !success, true, "Bad remote key fails through the network");
        called = false;
        request.opcode = RdmaOpcode::PUT_SIGNAL;
        request.signalAddress = 65536;
        pair.endpoint[0]->SubmitRdmaOperation(request, [&](const RdmaCompletion& c) {
            called = true;
            success = c.success;
        });
        pair.Run();
        NS_TEST_ASSERT_MSG_EQ(called && !success, true, "Failed put retires its signal child");
        NS_TEST_ASSERT_MSG_EQ(pair.Engine(1)->ReadWord(65536),
                              0,
                              "Failed put must not publish a signal");
    }
};

class RdmaOperationsSuite : public TestSuite
{
  public:
    RdmaOperationsSuite()
        : TestSuite("gpu-cluster-rdma-operations", Type::UNIT)
    {
        for (const auto& actor : {"cpu", "gpu"})
        {
            for (const auto& path : {"host", "gpudirect", "staged"})
            {
                AddTestCase(new RdmaReadOperationTest(actor, path), TestCase::Duration::QUICK);
            }
        }
        for (auto opcode : {RdmaOpcode::SEND, RdmaOpcode::SEND_IMM, RdmaOpcode::WRITE_IMM})
        {
            AddTestCase(new RdmaSendOperationTest(opcode), TestCase::Duration::QUICK);
        }
        for (const auto& path : {"host", "gpudirect", "staged"})
        {
            AddTestCase(new RdmaAtomicOperationTest(path), TestCase::Duration::QUICK);
            AddTestCase(new RdmaOrderedReadTest(path), TestCase::Duration::QUICK);
            AddTestCase(new RdmaSignalOperationTest(path), TestCase::Duration::QUICK);
            AddTestCase(new RdmaSignalOperationTest(path, 1), TestCase::Duration::QUICK);
        }
        AddTestCase(new RdmaFailureOperationTest, TestCase::Duration::QUICK);
    }
} g_rdmaOperationsSuite;
