/*
 * Copyright (c) 2007 University of Washington
 * Copyright (c) 2013 ResiliNets, ITTC, University of Kansas
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 */

/* BurstErrorModel additions
 *
 * Author: Truc Anh N. Nguyen   <annguyen@ittc.ku.edu>
 *         ResiliNets Research Group   https://resilinets.org/
 *         James P.G. Sterbenz <jpgs@ittc.ku.edu>, director
 */

#include "ns3/address.h"
#include "ns3/callback.h"
#include "ns3/double.h"
#include "ns3/error-model.h"
#include "ns3/mac48-address.h"
#include "ns3/node.h"
#include "ns3/packet.h"
#include "ns3/pointer.h"
#include "ns3/queue.h"
#include "ns3/rng-seed-manager.h"
#include "ns3/simple-channel.h"
#include "ns3/simple-net-device.h"
#include "ns3/simulator.h"
#include "ns3/string.h"
#include "ns3/test.h"

#include <sys/wait.h>
#include <unistd.h>

using namespace ns3;

static void
SendPacket(int num, Ptr<NetDevice> device, Address& addr)
{
    for (int i = 0; i < num; i++)
    {
        Ptr<Packet> pkt = Create<Packet>(1000); // 1000 dummy bytes of data
        device->Send(pkt, addr, 0);
    }
}

// Two nodes, two devices, one channel
static void
BuildSimpleTopology(Ptr<Node> a,
                    Ptr<Node> b,
                    Ptr<SimpleNetDevice> input,
                    Ptr<SimpleNetDevice> output,
                    Ptr<SimpleChannel> channel)
{
    ObjectFactory queueFactory;
    queueFactory.SetTypeId("ns3::DropTailQueue<Packet>");
    queueFactory.Set("MaxSize", StringValue("100000p")); // Much larger than we need
    Ptr<Queue<Packet>> queueA = queueFactory.Create<Queue<Packet>>();
    Ptr<Queue<Packet>> queueB = queueFactory.Create<Queue<Packet>>();

    input->SetQueue(queueA);
    output->SetQueue(queueB);
    a->AddDevice(input);
    b->AddDevice(output);
    input->SetAddress(Mac48Address::Allocate());
    input->SetChannel(channel);
    input->SetNode(a);
    output->SetChannel(channel);
    output->SetNode(b);
    output->SetAddress(Mac48Address::Allocate());
}

/**
 * @ingroup network-test
 * @ingroup tests
 *
 * ErrorModel unit tests.
 */
class ErrorModelSimple : public TestCase
{
  public:
    ErrorModelSimple();
    ~ErrorModelSimple() override;

  private:
    void DoRun() override;
    /**
     * Receive form a NetDevice
     * @param nd The NetDevice.
     * @param p The received packet.
     * @param protocol The protocol received.
     * @param addr The sender address.
     * @return True on success.
     */
    bool Receive(Ptr<NetDevice> nd, Ptr<const Packet> p, uint16_t protocol, const Address& addr);
    /**
     * Register a Drop
     * @param p The dropped packet
     */
    void DropEvent(Ptr<const Packet> p);

    uint32_t m_count; //!< The received packets counter.
    uint32_t m_drops; //!< The dropped packets counter.
};

// Add some help text to this case to describe what it is intended to test
ErrorModelSimple::ErrorModelSimple()
    : TestCase("ErrorModel and PhyRxDrop trace for SimpleNetDevice"),
      m_count(0),
      m_drops(0)
{
}

ErrorModelSimple::~ErrorModelSimple()
{
}

bool
ErrorModelSimple::Receive(Ptr<NetDevice> nd,
                          Ptr<const Packet> p,
                          uint16_t protocol,
                          const Address& addr)
{
    m_count++;
    return true;
}

void
ErrorModelSimple::DropEvent(Ptr<const Packet> p)
{
    m_drops++;
}

void
ErrorModelSimple::DoRun()
{
    // Set some arbitrary deterministic values
    RngSeedManager::SetSeed(7);
    RngSeedManager::SetRun(2);

    Ptr<Node> a = CreateObject<Node>();
    Ptr<Node> b = CreateObject<Node>();

    Ptr<SimpleNetDevice> input = CreateObject<SimpleNetDevice>();
    Ptr<SimpleNetDevice> output = CreateObject<SimpleNetDevice>();
    Ptr<SimpleChannel> channel = CreateObject<SimpleChannel>();
    BuildSimpleTopology(a, b, input, output, channel);

    output->SetReceiveCallback(MakeCallback(&ErrorModelSimple::Receive, this));
    Ptr<UniformRandomVariable> uv = CreateObject<UniformRandomVariable>();
    // Set this variable to a specific stream
    uv->SetStream(50);

    Ptr<RateErrorModel> em = CreateObject<RateErrorModel>();
    em->SetRandomVariable(uv);
    em->SetAttribute("ErrorRate", DoubleValue(0.001));
    em->SetAttribute("ErrorUnit", StringValue("ERROR_UNIT_PACKET"));

    // The below hooks will cause drops and receptions to be counted
    output->SetAttribute("ReceiveErrorModel", PointerValue(em));
    output->TraceConnectWithoutContext("PhyRxDrop",
                                       MakeCallback(&ErrorModelSimple::DropEvent, this));

    // Send 10000 packets
    Simulator::Schedule(Seconds(0), &SendPacket, 10000, input, output->GetAddress());

    Simulator::Run();
    Simulator::Destroy();

    // For this combination of values, we expect about 1 packet in 1000 to be
    // dropped.  For this specific RNG stream, we see 9991 receptions and 9 drops
    NS_TEST_ASSERT_MSG_EQ(m_count, 9991, "Wrong number of receptions.");
    NS_TEST_ASSERT_MSG_EQ(m_drops, 9, "Wrong number of drops.");
}

/**
 * @ingroup network-test
 * @ingroup tests
 *
 * BurstErrorModel unit tests.
 */
class BurstErrorModelSimple : public TestCase
{
  public:
    BurstErrorModelSimple();
    ~BurstErrorModelSimple() override;

  private:
    void DoRun() override;
    /**
     * Receive form a NetDevice
     * @param nd The NetDevice.
     * @param p The received packet.
     * @param protocol The protocol received.
     * @param addr The sender address.
     * @return True on success.
     */
    bool Receive(Ptr<NetDevice> nd, Ptr<const Packet> p, uint16_t protocol, const Address& addr);
    /**
     * Register a Drop
     * @param p The dropped packet
     */
    void DropEvent(Ptr<const Packet> p);

    uint32_t m_count; //!< The received packets counter.
    uint32_t m_drops; //!< The dropped packets counter.
};

// Add some help text to this case to describe what it is intended to test
BurstErrorModelSimple::BurstErrorModelSimple()
    : TestCase("ErrorModel and PhyRxDrop trace for SimpleNetDevice"),
      m_count(0),
      m_drops(0)
{
}

BurstErrorModelSimple::~BurstErrorModelSimple()
{
}

bool
BurstErrorModelSimple::Receive(Ptr<NetDevice> nd,
                               Ptr<const Packet> p,
                               uint16_t protocol,
                               const Address& addr)
{
    m_count++;
    return true;
}

void
BurstErrorModelSimple::DropEvent(Ptr<const Packet> p)
{
    m_drops++;
}

void
BurstErrorModelSimple::DoRun()
{
    // Set some arbitrary deterministic values
    RngSeedManager::SetSeed(5);
    RngSeedManager::SetRun(8);

    Ptr<Node> a = CreateObject<Node>();
    Ptr<Node> b = CreateObject<Node>();

    Ptr<SimpleNetDevice> input = CreateObject<SimpleNetDevice>();
    Ptr<SimpleNetDevice> output = CreateObject<SimpleNetDevice>();
    Ptr<SimpleChannel> channel = CreateObject<SimpleChannel>();
    BuildSimpleTopology(a, b, input, output, channel);

    output->SetReceiveCallback(MakeCallback(&BurstErrorModelSimple::Receive, this));
    Ptr<UniformRandomVariable> uv = CreateObject<UniformRandomVariable>();
    // Set this variable to a specific stream
    uv->SetStream(50);

    Ptr<BurstErrorModel> em = CreateObject<BurstErrorModel>();
    em->SetRandomVariable(uv);
    em->SetAttribute("ErrorRate", DoubleValue(0.01));

    // Assign the underlying error model random variables to specific streams
    em->AssignStreams(51);

    // The below hooks will cause drops and receptions to be counted
    output->SetAttribute("ReceiveErrorModel", PointerValue(em));
    output->TraceConnectWithoutContext("PhyRxDrop",
                                       MakeCallback(&BurstErrorModelSimple::DropEvent, this));

    // Send 10000 packets
    Simulator::Schedule(Seconds(0), &SendPacket, 10000, input, output->GetAddress());

    Simulator::Run();
    Simulator::Destroy();

    // With the burst error rate to be 0.01 and the burst size to be from 1 to 4,
    // we expect about 2.5 packets being dropped every 1000 packets.
    // That means for 10000 packets, we expect a total of about 250 packet drops.
    // For this specific RNG seed, we see 9740 receptions and 260 drops.
    NS_TEST_ASSERT_MSG_EQ(m_count, 9740, "Wrong number of receptions.");
    NS_TEST_ASSERT_MSG_EQ(m_drops, 260, "Wrong number of drops.");
}

/**
 * @ingroup network-test
 * @ingroup tests
 *
 * RateErrorModel domain and stability unit tests (codex round 51,
 * findings 6 and 9).
 */
class RateErrorModelRangeTest : public TestCase
{
  public:
    RateErrorModelRangeTest();
    ~RateErrorModelRangeTest() override = default;

  private:
    void DoRun() override;
};

RateErrorModelRangeTest::RateErrorModelRangeTest()
    : TestCase("RateErrorModel domain and tiny-BER stability")
{
}

void
RateErrorModelRangeTest::DoRun()
{
    // SetRate must reject a non-probability (codex round 51, finding 9):
    // a rate above 1 makes log1p(-rate) NaN, and NaN compares false —
    // every packet silently clean. NS_ABORT exits the process, so each
    // guard forks and checks the child's exit status — the same pattern
    // the ns-3 suite uses for fatal-path tests.
    auto expectAbort = [this](const std::string& what, void (*fn)()) {
        pid_t pid = fork();
        NS_TEST_ASSERT_MSG_EQ(pid >= 0, true, "fork must succeed for " + what);
        if (pid == 0)
        {
            fn(); // must abort; reaching the next line is the failure
            _exit(0);
        }
        int status = 0;
        waitpid(pid, &status, 0);
        const bool aborted = (WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT)
            || (WIFEXITED(status) && WEXITSTATUS(status) != 0);
        NS_TEST_ASSERT_MSG_EQ(aborted, true, what + " must abort");
    };

    expectAbort("SetRate(1.5)", []() {
        Ptr<RateErrorModel> em = CreateObject<RateErrorModel>();
        em->SetRate(1.5);
    });
    expectAbort("SetRate(-0.1)", []() {
        Ptr<RateErrorModel> em = CreateObject<RateErrorModel>();
        em->SetRate(-0.1);
    });
    expectAbort("ErrorRate=2.0 attribute", []() {
        Ptr<RateErrorModel> em = CreateObject<RateErrorModel>();
        em->SetAttribute("ErrorRate", DoubleValue(2.0));
    });

    // Tiny-BER byte stability (codex round 51, finding 6): a rate so small
    // that 1.0 - rate rounds back to 1.0 must still corrupt a large enough
    // packet. With the old pow(1 - rate, size) form, per collapsed to 0
    // for ANY size. Use a deterministic ranvar pinned to 0.5 so the
    // comparison against per is the only variable.
    {
        Ptr<RateErrorModel> em = CreateObject<RateErrorModel>();
        em->SetAttribute("ErrorUnit", StringValue("ERROR_UNIT_BYTE"));
        em->SetRate(1e-17);
        Ptr<UniformRandomVariable> uv = CreateObject<UniformRandomVariable>();
        uv->SetAttribute("Min", DoubleValue(0.5));
        uv->SetAttribute("Max", DoubleValue(0.5));
        em->SetRandomVariable(uv);

        // per = -expm1(size * log1p(-1e-17)) ~ size * 1e-17.
        // A 4 KiB packet: per ~ 4.1e-14 < 0.5  -> kept.
        Ptr<Packet> small = Create<Packet>(4096);
        bool corrupt = em->IsCorrupt(small);
        NS_TEST_EXPECT_MSG_EQ(corrupt, false, "tiny BER: 4 KiB packet survives a 0.5 draw");

        // A 1 TiB packet: per ~ 1.1e-5 < 0.5 -> still kept, but the old
        // form would also keep it (both round); the discriminating case is
        // a ranvar draw below per.
        Ptr<UniformRandomVariable> uvLow = CreateObject<UniformRandomVariable>();
        uvLow->SetAttribute("Min", DoubleValue(1e-6));
        uvLow->SetAttribute("Max", DoubleValue(1e-6));
        em->SetRandomVariable(uvLow);
        Ptr<Packet> large = Create<Packet>(110000000); // 110 MB: per ~ 1.1e-9
        corrupt = em->IsCorrupt(large);
        // Old pow form: per == 0 exactly -> 1e-6 < 0 false -> not corrupt.
        // Stable form: per ~ 1.1e-9; 1e-6 < 1.1e-9 false -> not corrupt.
        // Both agree here; the distinguishing assertion is below.
        NS_TEST_EXPECT_MSG_EQ(corrupt, false, "tiny BER: 110 MB packet survives a 1e-6 draw");

        // Draw 0.0: stable form reports corrupt iff per > 0. With per
        // collapsed to 0 the old form never corrupts; the stable form
        // corrupts a 110 MB packet because per ~ 1.1e-9 > 0.
        Ptr<UniformRandomVariable> uvZero = CreateObject<UniformRandomVariable>();
        uvZero->SetAttribute("Min", DoubleValue(0.0));
        uvZero->SetAttribute("Max", DoubleValue(0.0));
        em->SetRandomVariable(uvZero);
        corrupt = em->IsCorrupt(large);
        NS_TEST_EXPECT_MSG_EQ(corrupt,
                              true,
                              "tiny BER: 110 MB packet corrupt at a 0.0 draw (per > 0)");
    }

    // Endpoint sanity: rate exactly 1.0 corrupts every byte unit.
    {
        Ptr<RateErrorModel> em = CreateObject<RateErrorModel>();
        em->SetAttribute("ErrorUnit", StringValue("ERROR_UNIT_BYTE"));
        em->SetRate(1.0);
        Ptr<UniformRandomVariable> uv = CreateObject<UniformRandomVariable>();
        uv->SetAttribute("Min", DoubleValue(0.999999));
        uv->SetAttribute("Max", DoubleValue(0.999999));
        em->SetRandomVariable(uv);
        Ptr<Packet> p = Create<Packet>(1);
        NS_TEST_EXPECT_MSG_EQ(em->IsCorrupt(p), true, "rate 1.0 corrupts every unit");
    }
}

/**
 * @ingroup network-test
 * @ingroup tests
 *
 * @brief ErrorModel TestSuite
 *
 *  This is the start of an error model test suite.  For starters, this is
 *  just testing that the SimpleNetDevice is working but this can be
 *  extended to many more test cases in the future
 */
class ErrorModelTestSuite : public TestSuite
{
  public:
    ErrorModelTestSuite();
};

ErrorModelTestSuite::ErrorModelTestSuite()
    : TestSuite("error-model", Type::UNIT)
{
    AddTestCase(new ErrorModelSimple, TestCase::Duration::QUICK);
    AddTestCase(new BurstErrorModelSimple, TestCase::Duration::QUICK);
    AddTestCase(new RateErrorModelRangeTest, TestCase::Duration::QUICK);
}

// Do not forget to allocate an instance of this TestSuite
static ErrorModelTestSuite errorModelTestSuite; //!< Static variable for test initialization
