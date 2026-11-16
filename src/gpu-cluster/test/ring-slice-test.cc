/* SPDX-License-Identifier: GPL-2.0-only */
#include "ns3/boolean.h"
#include "ns3/config.h"
#include "ns3/fabric-endpoint.h"
#include "ns3/mac48-address.h"
#include "ns3/nccl-protocol-model.h"
#include "ns3/nccl-protocol-payload-builder.h"
#include "ns3/node-container.h"
#include "ns3/nvswitch.h"
#include "ns3/point-to-point-helper.h"
#include "ns3/ring-allreduce.h"
#include "ns3/simulator.h"
#include "ns3/string.h"
#include "ns3/test.h"

#include <algorithm>
#include <map>
#include <numeric>
#include <tuple>
#include <vector>

using namespace ns3;

namespace
{
struct SliceEvent
{
    char kind;
    uint64_t atNs;
    uint16_t rank;
    uint32_t loop, step, slice;
    uint64_t bytes;
};

void Record(std::vector<SliceEvent>* events, char kind, uint16_t rank,
            uint32_t loop, uint32_t step, uint32_t slice, uint64_t bytes)
{
    events->push_back({kind, static_cast<uint64_t>(Simulator::Now().GetNanoSeconds()),
                       rank, loop, step, slice, bytes});
}

struct RunResult
{
    bool completed{false};
    bool sliced{false};
    uint64_t durationNs{0}, sentBytes{0};
    uint32_t loops{0};
    std::vector<SliceEvent> events;
};

RunResult RunRingCase(uint16_t ranks, uint64_t bytes, const std::string& mode,
              uint64_t buffer = 4 * 1024 * 1024, uint32_t element = 1,
              uint8_t protocolId = 3, bool oneHost = true,
              std::vector<uint16_t> order = {})
{
    RunResult result;
    NodeContainer nodes, switches;
    nodes.Create(ranks);
    switches.Create(1);
    auto sw = CreateObject<NvSwitch>();
    sw->SetVoqDepth(100000);
    sw->SetArbitrationInterval(1);
    sw->SetCutThroughDelay(100);
    auto proto = CreateObject<NcclProtocolModel>();
    proto->SetForceProtocolId(protocolId);
    auto payload = CreateObject<NcclProtocolPayloadBuilder>();
    std::vector<Ptr<FabricEndpoint>> endpoints;
    std::vector<Mac48Address> macs;
    for (uint16_t rank = 0; rank < ranks; ++rank)
    {
        PointToPointHelper links;
        links.SetDeviceAttribute("DataRate", StringValue("100Gbps"));
        links.SetChannelAttribute("Delay", TimeValue(rank == 0 ? MicroSeconds(100) : MicroSeconds(1)));
        links.SetQueue("ns3::DropTailQueue<Packet>", "MaxSize", StringValue("100000p"));
        auto pair = links.Install(nodes.Get(rank), switches.Get(0));
        auto ep = CreateObject<FabricEndpoint>();
        ep->SetRank(rank);
        ep->SetNodeId(oneHost ? 0 : rank);
        ep->SetLocalRankRange(0, ranks);
        ep->AddNetDevice(pair.Get(0));
        ep->SetVcCredits(0, 100 * 1024 * 1024);
        ep->SetBulkChunkSize(4096);
        ep->SetBypassReorderBuffer(true);
        ep->SetProtocolModel(proto);
        ep->SetProtocolPayloadBuilder(payload);
        for (uint16_t dest = 0; dest < ranks; ++dest)
        {
            if (dest != rank)
            {
                ep->SetRoutingEntry(dest, 0);
            }
        }
        sw->AddPort(pair.Get(1));
        macs.push_back(Mac48Address::ConvertFrom(pair.Get(0)->GetAddress()));
        endpoints.push_back(ep);
    }
    if (order.empty())
    {
        order.resize(ranks);
        std::iota(order.begin(), order.end(), 0);
    }
    for (uint16_t index = 0; index < ranks; ++index)
    {
        endpoints[order[index]]->SetRingNeighbors(order[(index + 1) % ranks],
                                                 order[(index + ranks - 1) % ranks]);
    }
    for (uint16_t rank = 0; rank < ranks; ++rank)
    {
        sw->AddStaticRoute(macs[rank], rank);
        for (uint16_t dest = 0; dest < ranks; ++dest)
        {
            if (dest != rank)
            {
                endpoints[rank]->SetNeighborMac(dest, macs[dest]);
            }
        }
        endpoints[rank]->Initialize();
    }
    auto ring = CreateObject<RingAllReduce>();
    ring->SetAttribute("UseTransactionModel", BooleanValue(true));
    ring->SetProgressMode(mode);
    ring->SetSimpleBufferBytes(buffer);
    ring->SetElementBytes(element);
    ring->SetStartupDelay(NanoSeconds(1000));
    ring->TraceConnectWithoutContext("SliceIssued", MakeBoundCallback(&Record, &result.events, 'I'));
    ring->TraceConnectWithoutContext("SliceDelivered", MakeBoundCallback(&Record, &result.events, 'D'));
    ring->TraceConnectWithoutContext("SliceConsumed", MakeBoundCallback(&Record, &result.events, 'C'));
    ring->SetCompletionCallback([&result](uint64_t ns) {
        result.completed = true;
        result.durationNs = ns;
    });
    ring->Initialize(ranks, bytes, endpoints);
    ring->Start();
    Simulator::Stop(MilliSeconds(50));
    Simulator::Run();
    result.sliced = ring->UsesSliceProgress();
    result.loops = ring->GetPipelineLoopCount();
    result.sentBytes = ring->GetSentDataBytes();
    ring->Dispose();
    Simulator::Destroy();
    return result;
}

class SliceCoverageTest : public TestCase
{
  public:
    SliceCoverageTest() : TestCase("SIMPLE slice ring conserves bytes across loops, tails and reordered ranks") {}
    void DoRun() override
    {
        constexpr uint64_t buffer = 256 * 1024;
        constexpr uint16_t ranks = 5;
        for (uint64_t bytes : {uint64_t{0}, uint64_t{1}, uint64_t{15}, uint64_t{262145}, uint64_t{1048583}})
        {
            auto run = RunRingCase(ranks, bytes, "slice", buffer, 1, 3, true, {0, 2, 4, 1, 3});
            NS_TEST_ASSERT_MSG_EQ(run.completed, true, "The tail/empty-slice schedule must finish");
            const uint64_t expected = 2 * (ranks - 1) * bytes;
            NS_TEST_ASSERT_MSG_EQ(run.sentBytes, expected, "No payload may be lost or fabricated");
            uint64_t issued = 0, received = 0;
            for (const auto& event : run.events)
            {
                if (event.kind == 'I') issued += event.bytes;
                if (event.kind == 'D') received += event.bytes;
            }
            NS_TEST_ASSERT_MSG_EQ(issued, expected, "Issued slice coverage must equal ring traffic");
            NS_TEST_ASSERT_MSG_EQ(received, expected, "Completion must cover received payload");
            const uint64_t capacity = ranks * (buffer / 8 * 4);
            NS_TEST_ASSERT_MSG_EQ(run.loops, bytes / capacity + (bytes % capacity != 0),
                                  "SIMPLE loops are bounded by its configured buffer");
        }
        const auto local = RunRingCase(1, 12345, "slice");
        NS_TEST_ASSERT_MSG_EQ(local.completed, true, "A single rank is a local no-op");
        NS_TEST_ASSERT_MSG_EQ(local.sentBytes, 0, "Single-rank allreduce has no wire traffic");
    }
};

class SliceProgressTest : public TestCase
{
  public:
    SliceProgressTest() : TestCase("SIMPLE forwards an input slice before its whole chunk arrives") {}
    void DoRun() override
    {
        // Two ranks make the legacy phase reversal and NCCL's forward ring
        // the same physical route, isolating progress granularity.
        const auto sliced = RunRingCase(2, 4 * 1024 * 1024, "slice");
        const auto chunk = RunRingCase(2, 4 * 1024 * 1024, "chunk");
        NS_TEST_ASSERT_MSG_EQ(sliced.completed && chunk.completed, true, "Both modes must finish");
        using Key = std::tuple<uint16_t, uint32_t, uint32_t>;
        std::map<Key, uint64_t> firstForward, chunkReceived;
        for (const auto& event : sliced.events)
        {
            if (event.kind == 'I' && event.step == 1 && event.slice == 0)
                firstForward[{event.rank, event.loop, 0}] = event.atNs;
            if (event.kind == 'D' && event.step == 0)
                chunkReceived[{event.rank, event.loop, 0}] =
                    std::max(chunkReceived[{event.rank, event.loop, 0}], event.atNs);
        }
        NS_TEST_ASSERT_MSG_EQ(firstForward.size(), 2, "Both ranks must issue a forwarded first slice");
        for (const auto& [key, time] : firstForward)
            NS_TEST_ASSERT_MSG_LT(time, chunkReceived.at(key), "Forwarding must not wait for the whole input chunk");
        NS_TEST_ASSERT_MSG_LT(sliced.durationNs, chunk.durationNs,
                              "Partial progress must reduce slow-route pipeline bubbles");
        const auto smallSlice = RunRingCase(2, 4096, "slice");
        const auto smallChunk = RunRingCase(2, 4096, "chunk");
        NS_TEST_ASSERT_MSG_EQ(smallSlice.durationNs, smallChunk.durationNs,
                              "A single nonempty slice must preserve the same small-message dependencies");
    }
};

class SliceWindowTest : public TestCase
{
  public:
    SliceWindowTest() : TestCase("SIMPLE slice dependencies preserve issue order and four send slots") {}
    void DoRun() override
    {
        constexpr uint16_t ranks = 4;
        const auto run = RunRingCase(ranks, 2 * 1024 * 1024 + 128, "slice", 256 * 1024);
        NS_TEST_ASSERT_MSG_EQ(run.completed, true, "Multi-loop pipeline must finish");
        using Key = std::tuple<uint16_t, uint32_t, uint32_t, uint32_t>;
        std::map<Key, uint64_t> received, consumed;
        std::vector<int> inflight(ranks, 0);
        for (const auto& event : run.events)
        {
            Key key{event.rank, event.loop, event.step, event.slice};
            if (event.kind == 'D') received[key] = event.atNs;
            if (event.kind == 'C')
            {
                consumed[key] = event.atNs;
                const uint16_t source = (event.rank + ranks - 1) % ranks;
                --inflight[source];
                NS_TEST_ASSERT_MSG_GT_OR_EQ(inflight[source], 0, "A slot cannot be consumed before issue");
            }
            if (event.kind == 'I')
            {
                ++inflight[event.rank];
                NS_TEST_ASSERT_MSG_LT_OR_EQ(inflight[event.rank], 4, "NCCL_STEPS/SLICE_STEPS bounds the send window");
            }
        }
        for (const auto& event : run.events)
        {
            if (event.kind != 'I') continue;
            if (event.step > 0)
            {
                const Key input{event.rank, event.loop, event.step - 1, event.slice};
                NS_TEST_ASSERT_MSG_EQ(received.count(input), 1, "Each forwarded slice needs its own input");
                NS_TEST_ASSERT_MSG_GT_OR_EQ(event.atNs, received.at(input), "No forwarding before matching input delivery");
            }
            else if (event.loop > 0)
            {
                for (uint32_t slice = 0; slice < 2; ++slice)
                {
                    const Key final{event.rank, event.loop - 1, 2 * (ranks - 1) - 1, slice};
                    NS_TEST_ASSERT_MSG_GT_OR_EQ(event.atNs, consumed.at(final), "Next loop waits for this rank's final receive");
                }
            }
        }
        for (int outstanding : inflight)
            NS_TEST_ASSERT_MSG_EQ(outstanding, 0, "Completion must drain all SIMPLE slots");
    }
};

class SliceAlignmentTest : public TestCase
{
  public:
    SliceAlignmentTest() : TestCase("SIMPLE slice alignment uses element width rather than packet size") {}
    void DoRun() override
    {
        const auto run = RunRingCase(4, 8200, "slice", 128 * 1024, 2);
        NS_TEST_ASSERT_MSG_EQ(run.completed, true, "BF16-width slices must complete");
        std::vector<uint64_t> sizes;
        for (const auto& event : run.events)
            if (event.kind == 'I' && event.rank == 0 && event.loop == 0 && event.step == 0)
                sizes.push_back(event.bytes);
        NS_TEST_ASSERT_MSG_EQ(sizes.size(), 2, "A logical chunk has two SIMPLE slice positions");
        // 2050 bytes = 1025 elements: ceil(1025/(16*2))*16 = 528
        // elements, hence 1056 bytes; the exact logical remainder is 994.
        NS_TEST_ASSERT_MSG_EQ(sizes[0], 1056, "SIMPLE aligns a slice to 16 elements");
        NS_TEST_ASSERT_MSG_EQ(sizes[1], 994, "Alignment must not fabricate padding traffic");
    }
};

class SliceEligibilityTest : public TestCase
{
  public:
    SliceEligibilityTest() : TestCase("Automatic slice progress is limited to single-host NCCL SIMPLE") {}
    void DoRun() override
    {
        auto simple = RunRingCase(2, 4096, "auto");
        auto ll = RunRingCase(2, 4096, "auto", 4 * 1024 * 1024, 1, 1);
        auto remote = RunRingCase(2, 4096, "auto", 4 * 1024 * 1024, 1, 3, false);
        NS_TEST_ASSERT_MSG_EQ(simple.completed && ll.completed && remote.completed, true,
                              "All supported legacy/auto cases must complete");
        NS_TEST_ASSERT_MSG_EQ(simple.sliced, true, "SIMPLE on one host uses slices");
        NS_TEST_ASSERT_MSG_EQ(ll.sliced, false, "LL is not a SIMPLE slice schedule");
        NS_TEST_ASSERT_MSG_EQ(remote.sliced, false, "Cross-host scheduling remains explicit and unchanged");
    }
};

class RingSliceTestSuite : public TestSuite
{
  public:
    RingSliceTestSuite() : TestSuite("gpu-cluster-ring-slices", Type::UNIT)
    {
        AddTestCase(new SliceCoverageTest, TestCase::Duration::QUICK);
        AddTestCase(new SliceProgressTest, TestCase::Duration::QUICK);
        AddTestCase(new SliceWindowTest, TestCase::Duration::QUICK);
        AddTestCase(new SliceAlignmentTest, TestCase::Duration::QUICK);
        AddTestCase(new SliceEligibilityTest, TestCase::Duration::QUICK);
    }
} g_ringSliceTests;
} // namespace
