/* SPDX-License-Identifier: GPL-2.0-only */
#include "ns3/dcqcn-rate-machine.h"
#include "ns3/double.h"
#include "ns3/fabric-endpoint.h"
#include "ns3/fec-model.h"
#include "ns3/gateway-endpoint.h"
#include "ns3/llr-manager.h"
#include "ns3/node-container.h"
#include "ns3/point-to-point-helper.h"
#include "ns3/point-to-point-net-device.h"
#include "ns3/queue.h"
#include "ns3/rdma-operation-engine.h"
#include "ns3/rttcc-rate-machine.h"
#include "ns3/string.h"
#include "ns3/test.h"
#include "ns3/time-bounds.h"

#include <array>
#include <cfenv>
#include <cmath>
#include <cstdlib>
#include <limits>

using namespace ns3;

namespace
{
class FloatingPointChecks
{
  public:
    FloatingPointChecks()
    {
        std::feclearexcept(FE_ALL_EXCEPT);
#if defined(__GLIBC__)
        m_previous = fegetexcept();
        if (std::getenv("PROTOBRIDGE_TIME_TEST_TRAPS"))
        {
            feenableexcept(FE_OVERFLOW | FE_INVALID | FE_DIVBYZERO);
        }
#endif
    }

    ~FloatingPointChecks()
    {
#if defined(__GLIBC__)
        fedisableexcept(FE_ALL_EXCEPT);
        std::feclearexcept(FE_ALL_EXCEPT);
        feenableexcept(m_previous);
#endif
    }

  private:
    int m_previous{0};
};

class TimeConversionCase : public TestCase
{
  public:
    TimeConversionCase() : TestCase("Bounded conversions preserve the native Time range") {}

    void DoRun() override
    {
        const char* selected = std::getenv("PROTOBRIDGE_TIME_TEST_UNIT");
        const auto unit = selected ? static_cast<Time::Unit>(std::stoi(selected)) : Time::NS;
        Time::SetResolution(unit);
        FloatingPointChecks checks;
        constexpr int64_t maximum = std::numeric_limits<int64_t>::max();
        // Independent integer/Fraction reference values for 2^63 nanoseconds.
        constexpr std::array<int64_t, 10> ns63{
            292, 106751, 2562047, 153722867, 9223372036, 9223372036854,
            9223372036854775, maximum, maximum, maximum};
        NS_TEST_ASSERT_MSG_EQ(TimeBounds::FromNanoseconds(TimeBounds::Wide{1} << 63).GetTimeStep(),
                              ns63[unit], "Unsigned nanoseconds must be checked before scaling");
        for (uint64_t ns : {uint64_t{1} << 61, (uint64_t{1} << 61) + 1})
        {
            if (unit == Time::PS || unit == Time::FS)
            {
                NS_TEST_ASSERT_MSG_EQ(TimeBounds::FromNanoseconds(ns).GetTimeStep(), maximum,
                                      "An overflowing nanosecond product must not become zero");
            }
        }
        for (double seconds : {0.0, 1e-12, 2.5e-9, 1e-6, 1.2345, 60.0, 3600.0})
        {
            NS_TEST_ASSERT_MSG_EQ(TimeBounds::FromSeconds(seconds), Seconds(seconds),
                                  "Ordinary ns-3 fixed-point rounding must be preserved");
        }
        constexpr std::array<int64_t, 10> seconds64{
            584942417355, 213503982334601, 5124095576030431, 307445734561825860,
            maximum, maximum, maximum, maximum, maximum, maximum};
        NS_TEST_ASSERT_MSG_EQ(TimeBounds::FromSeconds(0x1p64).GetTimeStep(), seconds64[unit],
                              "Large seconds can fit coarse native units");
        RdmaConfig config;
        config.dataPath = "gpudirect";
        config.rnrRetry = Time(1);
        config.hbmReadGbps = 0x1p-64;
        auto model = CreateObject<RdmaSubmissionModel>();
        model->Configure(config);
        NS_TEST_ASSERT_MSG_EQ(model->ReserveSourceRead(125000000).GetTimeStep(), seconds64[unit],
                              "The real RDMA reservation must retain representable services");
        model->Dispose();

        std::string mode = "detailed";
        RdmaConfig profile;
        profile.rnrRetry = Time(1);
        ApplyRdmaProfile({{"rdmaCpuWqeBuildNs", "9223372036854775808"}}, mode, profile);
        NS_TEST_ASSERT_MSG_EQ(profile.cpuWqeBuild.GetTimeStep(), ns63[unit],
                              "Profile parsing must retain unsigned nanosecond inputs");
        NS_TEST_ASSERT_MSG_EQ(TimeBounds::FromRate(16384.0, 1e-300), Time::Max(),
                              "Reject an oversized quotient before division");
        NS_TEST_ASSERT_MSG_EQ(TimeBounds::FromSeconds(1e300), Time::Max(),
                              "An oversized seconds conversion must not abort");

        if (unit == Time::NS)
        {
            const TimeBounds::Wide wireBytes = TimeBounds::Wide{2305843010287435775ULL} + 39;
            NS_TEST_ASSERT_MSG_EQ(TimeBounds::FromBitRate(wireBytes * 8, 200000000000ULL).GetTimeStep(),
                                  92233720411497433LL,
                                  "FEC bytes must retain their high bits through serialization");
        }
        if (unit == Time::PS || unit == Time::FS)
        {
            auto pacer = CreateObject<DcqcnRateMachine>();
            pacer->SetAttribute("PerQpCeilingGbps", DoubleValue(0x1p-58));
            NS_TEST_ASSERT_MSG_EQ(pacer->ShapingDelay(1, 1), Time(0), "First chunk is ready now");
            NS_TEST_ASSERT_MSG_EQ(pacer->ShapingDelay(1, 1), Time::Max(),
                                  "An oversized slot must not wrap to zero");
            pacer->Dispose();
        }
        NS_TEST_ASSERT_MSG_EQ(std::fetestexcept(FE_OVERFLOW | FE_INVALID | FE_DIVBYZERO), 0,
                              "Boundary arithmetic must be safe with FP traps enabled");
        Simulator::Destroy();
    }
};

class RateBoundaryCase : public TestCase
{
  public:
    RateBoundaryCase() : TestCase("Finite high rates remain paced without floating-point overflow") {}

    void DoRun() override
    {
        FloatingPointChecks checks;
        auto ceiling = CreateObject<DcqcnRateMachine>();
        ceiling->SetAttribute("PerQpCeilingGbps", DoubleValue(1e11));
        for (int i = 0; i < 3; ++i)
        {
            NS_TEST_ASSERT_MSG_EQ(ceiling->ShapingDelay(1, 1).GetTimeStep(), i,
                                  "A valid ceiling retains the minimum one-nanosecond slot");
        }
        auto dcqcn = CreateObject<DcqcnRateMachine>();
        dcqcn->SetAttribute("MaxRateBps", DoubleValue(1e300));
        dcqcn->OnCnp(1);
        NS_TEST_ASSERT_MSG_EQ(dcqcn->ShapingDelay(1024, 1), Time(0), "High finite DCQCN rate");
        dcqcn->Dispose();
        auto rttcc = CreateObject<RttccRateMachine>();
        rttcc->SetAttribute("LineRateGbps", DoubleValue(1e292));
        rttcc->SetAttribute("TargetRtt", TimeValue(NanoSeconds(1)));
        uint32_t seq;
        NS_TEST_ASSERT_MSG_EQ(rttcc->MaybeStartProbe(1, seq), true, "Start a public RTT probe");
        Simulator::Schedule(NanoSeconds(100), [this, rttcc, seq] {
            rttcc->OnProbeResponse(1, seq);
            NS_TEST_ASSERT_MSG_EQ(rttcc->ShapingDelay(1024, 1), Time(0), "High finite RTTCC rate");
        });
        Simulator::Run();
        NS_TEST_ASSERT_MSG_EQ(std::fetestexcept(FE_OVERFLOW | FE_INVALID | FE_DIVBYZERO), 0,
                              "Neither the pre-guard nor the rate product may overflow");
        ceiling->Dispose();
        rttcc->Dispose();
        Simulator::Destroy();
    }
};

class ReceivePostBoundaryCase : public TestCase
{
  public:
    ReceivePostBoundaryCase() : TestCase("Rejected receive posts fail without consuming RQ slots") {}

    void DoRun() override
    {
        RdmaConfig config;
        config.receiveQueueDepth = 2;
        config.receivePost = Time::Max();
        auto model = CreateObject<RdmaSubmissionModel>();
        model->Configure(config);
        auto engine = CreateObject<RdmaOperationEngine>();
        engine->Configure(0, model,
                          [](uint16_t, Ptr<Packet>, const RdmaOperationTag&, uint32_t,
                             uint16_t, uint8_t, uint32_t) {},
                          [](const FabricHeader&) {}, [](const FabricHeader&, bool) {});
        engine->RegisterMemory(0, 256);
        int failures = 0;
        for (uint64_t id : {1, 2})
        {
            engine->PostReceive(id, 0, 8, [&](const RdmaReceiveCompletion& result) {
                failures += !result.success;
            });
        }
        NS_TEST_ASSERT_MSG_EQ(failures, 2, "Both rejected posts must retain their callbacks");
        config.receivePost = Time(0);
        model->Configure(config);
        for (uint64_t id : {3, 4})
        {
            engine->PostReceive(id, 0, 8, [](const RdmaReceiveCompletion&) {});
        }
        Simulator::Run();
        NS_TEST_ASSERT_MSG_EQ(engine->GetStats().postedReceives, 4, "Count all valid post attempts");
        NS_TEST_ASSERT_MSG_EQ(engine->GetStats().receiveCompletions, 2, "Count failed completions");
        engine->Dispose();
        model->Dispose();
        Simulator::Destroy();
    }
};

class EgressBoundaryCase : public TestCase
{
  public:
    explicit EgressBoundaryCase(int scenario)
        : TestCase("P2P egress includes serialization, propagation, gap and queued frames: " +
                   std::to_string(scenario)), m_scenario(scenario) {}

    void MacTx(Ptr<const Packet>) { ++m_macTx; }
    void PhyTx(Ptr<const Packet>) { ++m_phyTx; }
    void Rx(Ptr<const Packet>)
    {
        ++m_rx;
        m_negativeRx |= Simulator::Now().IsStrictlyNegative();
    }
    bool Sink(Ptr<NetDevice>, Ptr<const Packet>, uint16_t, const Address&) { return true; }

    void DoRun() override
    {
        NodeContainer nodes;
        nodes.Create(2);
        PointToPointHelper link;
        link.SetDeviceAttribute("DataRate", StringValue("200Gbps"));
        link.SetDeviceAttribute("InterframeGap", TimeValue(Time(m_scenario == 2 ? 4 : 0)));
        link.SetChannelAttribute("Delay", TimeValue(Time(m_scenario == 1 ? 4 : 0)));
        auto devices = link.Install(nodes);
        devices.Get(1)->SetReceiveCallback(MakeCallback(&EgressBoundaryCase::Sink, this));
        devices.Get(0)->TraceConnectWithoutContext("MacTx", MakeCallback(&EgressBoundaryCase::MacTx, this));
        devices.Get(0)->TraceConnectWithoutContext("PhyTxBegin", MakeCallback(&EgressBoundaryCase::PhyTx, this));
        devices.Get(1)->TraceConnectWithoutContext("MacRx", MakeCallback(&EgressBoundaryCase::Rx, this));
        const int remaining = m_scenario == 0 ? 1 : m_scenario == 3 ? 5 : 7;
        Simulator::Schedule(Time::Max() - Time(remaining), [this, devices] {
            m_accepted = devices.Get(0)->Send(Create<Packet>(m_scenario == 3 ? 64 : 103),
                                              Mac48Address::GetBroadcast(), 0x0800);
            if (m_scenario == 3)
            {
                m_accepted &= devices.Get(0)->Send(Create<Packet>(64),
                                                  Mac48Address::GetBroadcast(), 0x0800);
            }
        });
        Simulator::Run();
        NS_TEST_ASSERT_MSG_EQ(m_negativeRx, false, "No receive event may wrap into the past");
        NS_TEST_ASSERT_MSG_EQ(m_phyTx, m_scenario == 3 ? 1 : 0, "Check again at actual transmit start");
        NS_TEST_ASSERT_MSG_EQ(m_rx, m_scenario == 3 ? 1 : 0, "Only the first queued frame can finish");
        NS_TEST_ASSERT_MSG_EQ(m_accepted, m_scenario == 3, "Reject unrepresentable direct sends");
        NS_TEST_ASSERT_MSG_EQ(m_macTx, m_scenario == 3 ? 2 : 0, "Direct rejection precedes MacTx");
        NS_TEST_ASSERT_MSG_EQ(DynamicCast<PointToPointNetDevice>(devices.Get(0))->GetQueue()->GetNPackets(),
                              0, "Discarded queued frames must not strand the queue");
        NS_TEST_ASSERT_MSG_EQ(Simulator::Now() < Time::Max(), true, "Never execute at the saturation marker");
        Simulator::Destroy();
    }

  private:
    int m_scenario;
    int m_macTx{0}, m_phyTx{0}, m_rx{0};
    bool m_accepted{false}, m_negativeRx{false};
};

class NegativeDelayCase : public TestCase
{
  public:
    NegativeDelayCase() : TestCase("Negative timing attributes are rejected before scheduling") {}
    void DoRun() override
    {
        const TimeValue negative(Time(-1));
        for (const auto& attribute : {"TFast", "TAi"})
        {
            NS_TEST_ASSERT_MSG_EQ(CreateObject<DcqcnRateMachine>()->SetAttributeFailSafe(attribute, negative),
                                  false, "Reject a negative congestion-control timer");
        }
        for (const auto& attribute : {"TargetRtt", "ProbeInterval"})
        {
            NS_TEST_ASSERT_MSG_EQ(CreateObject<RttccRateMachine>()->SetAttributeFailSafe(attribute, negative),
                                  false, "Reject a negative RTT timer");
        }
        NS_TEST_ASSERT_MSG_EQ(CreateObject<GatewayEndpoint>()->SetAttributeFailSafe("GatewayDelay", negative),
                              false, "Reject negative gateway latency");
        NS_TEST_ASSERT_MSG_EQ(CreateObject<FabricEndpoint>()->SetAttributeFailSafe("MemoryLatency", negative),
                              false, "Reject negative memory latency");
        NS_TEST_ASSERT_MSG_EQ(CreateObject<LlrManager>()->SetAttributeFailSafe("SourceReloadLatency", negative),
                              false, "Reject negative reload latency");
        Simulator::Destroy();
    }
};

class TimeConversionsSuite : public TestSuite
{
  public:
    TimeConversionsSuite() : TestSuite("gpu-cluster-time-conversions", Type::UNIT)
    {
        AddTestCase(new TimeConversionCase(), Duration::QUICK);
    }
} g_timeConversionsSuite;

class TimeSafetySuite : public TestSuite
{
  public:
    TimeSafetySuite() : TestSuite("gpu-cluster-time-safety", Type::UNIT)
    {
        AddTestCase(new RateBoundaryCase(), Duration::QUICK);
        AddTestCase(new ReceivePostBoundaryCase(), Duration::QUICK);
        for (int scenario = 0; scenario < 4; ++scenario)
        {
            AddTestCase(new EgressBoundaryCase(scenario), Duration::QUICK);
        }
        AddTestCase(new NegativeDelayCase(), Duration::QUICK);
    }
} g_timeSafetySuite;
} // namespace
