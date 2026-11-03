/* SPDX-License-Identifier: GPL-2.0-only */
#include "ns3/pcie-fabric.h"
#include "ns3/rdma-submission-model.h"
#include "ns3/simulator.h"
#include "ns3/test.h"

#include <algorithm>
#include <vector>

using namespace ns3;

class PcieHeaderTest : public TestCase
{
  public:
    PcieHeaderTest()
        : TestCase("PCIe TLP header and unaligned byte-enable round trips")
    {
    }

    void DoRun() override
    {
        for (auto type : {PcieTlpType::READ, PcieTlpType::WRITE, PcieTlpType::COMPLETION})
        {
            for (uint32_t offset : {0u, 1u, 2u, 3u})
            {
                for (uint32_t bytes : {1u, 2u, 3u, 128u, 253u})
                {
                    PcieTlpHeader h;
                    h.type = type;
                    h.address = (1ull << 42) + offset;
                    h.bytes = bytes;
                    h.requester = 37;
                    h.completer = 11;
                    h.tag = 255;
                    h.byteCount = bytes;
                    h.relaxed = true;
                    auto packet = Create<Packet>(h.PayloadBytesOnWire());
                    packet->AddHeader(h);
                    PcieTlpHeader got;
                    packet->RemoveHeader(got);
                    NS_TEST_ASSERT_MSG_EQ(got.bytes, bytes, "Valid byte count survives padding");
                    NS_TEST_ASSERT_MSG_EQ(got.address,
                                          type == PcieTlpType::COMPLETION ? offset : h.address,
                                          "Request address or completion lower address");
                    NS_TEST_ASSERT_MSG_EQ(got.requester, 37, "Requester ID");
                    NS_TEST_ASSERT_MSG_EQ(got.tag, 255, "Tag");
                    NS_TEST_ASSERT_MSG_EQ(got.relaxed, true, "RO attribute");
                    NS_TEST_ASSERT_MSG_EQ(packet->GetSize(),
                                          h.PayloadBytesOnWire(),
                                          "Wire payload padding");
                }
            }
        }
    }
};

class PcieCreditTest : public TestCase
{
  public:
    PcieCreditTest()
        : TestCase("PCIe finite queues and six credit pools recover without packet loss")
    {
    }

    void DoRun() override
    {
        PcieConfig c;
        c.queuePackets = 2;
        c.credits = {1, 16, 1, 0, 1, 16};
        c.creditUpdate = NanoSeconds(500);
        auto f = CreateObject<PcieFabric>();
        f->Configure(c);
        auto a = f->AddEndpoint("nic"), s = f->AddSwitch("switch");
        auto b = f->AddEndpoint("gpu", {}, [](uint32_t) { return NanoSeconds(100); });
        f->AddLink(a, s);
        f->AddLink(s, b);
        bool done = false;
        f->Write(a, b, 8192, [&] { done = true; });
        Simulator::Run();
        NS_TEST_ASSERT_MSG_EQ(done, true, "All posted writes become memory-visible");
        NS_TEST_ASSERT_MSG_EQ(f->GetStats().writeTlps, 32, "MPS splitting");
        NS_TEST_ASSERT_MSG_GT(f->GetStats().creditStalls, 0, "Credits actually stop transmission");
        NS_TEST_ASSERT_MSG_GT(f->GetStats().flowUpdates, 0, "UpdateFC packets restore credits");
        NS_TEST_ASSERT_MSG_EQ(f->GetStats().peakQueuePackets <= 2, true, "Bounded output queues");
        NS_TEST_ASSERT_MSG_EQ(f->HasPending(), false, "Tails, link ACKs and flow updates drain");
        f->Dispose();
        Simulator::Destroy();
    }
};

class PcieReadWindowTest : public TestCase
{
  public:
    PcieReadWindowTest()
        : TestCase("PCIe read tags and outstanding byte windows limit throughput")
    {
    }

    void Run(uint32_t tags, uint32_t bytesLimit, Time& finish)
    {
        PcieConfig c;
        c.tags = tags;
        c.maxReadBytes = bytesLimit;
        c.completionBufferBytes = bytesLimit;
        c.linkLatency = NanoSeconds(300);
        auto f = CreateObject<PcieFabric>();
        f->Configure(c);
        auto a = f->AddEndpoint("nic"), b = f->AddEndpoint("gpu");
        f->AddLink(a, b);
        bool done = false;
        f->Read(a, b, 8192, [&] {
            done = true;
            finish = Simulator::Now();
        });
        Simulator::Run();
        NS_TEST_ASSERT_MSG_EQ(done, true, "All split completions reassemble");
        NS_TEST_ASSERT_MSG_EQ(f->GetStats().readRequests, 16, "MRRS splitting");
        NS_TEST_ASSERT_MSG_EQ(f->GetStats().completionTlps, 32, "Completion MPS splitting");
        NS_TEST_ASSERT_MSG_EQ(f->GetStats().peakOutstandingReads <= tags, true, "Tag limit");
        NS_TEST_ASSERT_MSG_EQ(f->GetStats().peakOutstandingReadBytes <= bytesLimit,
                              true,
                              "Byte limit");
        f->Dispose();
        Simulator::Destroy();
    }

    void DoRun() override
    {
        Time serial, parallel, bytesBound;
        Run(1, 8192, serial);
        Run(16, 8192, parallel);
        Run(16, 512, bytesBound);
        NS_TEST_ASSERT_MSG_LT(parallel, serial, "More tags cover read round-trip latency");
        NS_TEST_ASSERT_MSG_EQ(bytesBound, serial, "One-MRRS byte window behaves like one tag");
    }
};

class PciePathTest : public TestCase
{
  public:
    PciePathTest()
        : TestCase("PCIe P2P stays below root uplink and host traffic shares it")
    {
    }

    void DoRun() override
    {
        auto f = CreateObject<PcieFabric>();
        PcieConfig c;
        f->Configure(c);
        auto root = f->AddEndpoint("host"), s = f->AddSwitch("switch");
        auto a = f->AddEndpoint("gpu0"), b = f->AddEndpoint("nic0"), d = f->AddEndpoint("gpu1");
        f->AddLink(root, s);
        f->AddLink(s, a);
        f->AddLink(s, b);
        f->AddLink(s, d);
        uint32_t rootHops = 0;
        uint32_t done = 0;
        f->SetObserver([&](const PcieTraceEvent& e) {
            if (e.event == "tx" && (e.from == root || e.to == root))
            {
                ++rootHops;
            }
        });
        f->Write(a, b, 8192, [&] { ++done; });
        Simulator::Run();
        NS_TEST_ASSERT_MSG_EQ(rootHops, 0, "GPU-to-NIC P2P does not consume CPU uplink");
        Time first, last;
        f->Write(a, root, 8192, [&] {
            ++done;
            first = Simulator::Now();
        });
        f->Write(d, root, 8192, [&] {
            ++done;
            last = Simulator::Now();
        });
        Simulator::Run();
        NS_TEST_ASSERT_MSG_EQ(done, 3, "Concurrent host transfers complete");
        NS_TEST_ASSERT_MSG_EQ(rootHops, 64, "Both flows use the same uplink");
        NS_TEST_ASSERT_MSG_EQ(first != last, true, "Shared uplink serializes concurrent flows");
        f->Dispose();
        Simulator::Destroy();
    }
};

class PciePostedTest : public TestCase
{
  public:
    PciePostedTest()
        : TestCase("PCIe link ACK precedes slow posted-write visibility without a Completion TLP")
    {
    }

    void DoRun() override
    {
        auto f = CreateObject<PcieFabric>();
        PcieConfig c;
        c.ackEvery = 1;
        f->Configure(c);
        auto a = f->AddEndpoint("nic");
        auto b = f->AddEndpoint("gpu", {}, [](uint32_t) { return NanoSeconds(500); });
        f->AddLink(a, b);
        Time ack, visible;
        f->SetObserver([&](const PcieTraceEvent& e) {
            if (e.event == "link_ack")
            {
                ack = e.time;
            }
        });
        f->Write(a, b, 16, [&] { visible = Simulator::Now(); });
        Simulator::Run();
        NS_TEST_ASSERT_MSG_EQ(ack < visible,
                              true,
                              "ACK confirms receipt, memory can still be busy");
        NS_TEST_ASSERT_MSG_EQ(f->GetStats().completionTlps, 0, "MWr is posted");
        f->Dispose();
        Simulator::Destroy();
    }
};

class PcieOrderingTest : public TestCase
{
  public:
    PcieOrderingTest()
        : TestCase("PCIe posted traffic passes credit-blocked non-posted requests")
    {
    }

    void DoRun() override
    {
        auto f = CreateObject<PcieFabric>();
        PcieConfig c;
        c.credits[2] = 1;
        c.creditUpdate = NanoSeconds(1000);
        f->Configure(c);
        auto a = f->AddEndpoint("nic"), b = f->AddEndpoint("gpu");
        f->AddLink(a, b);
        std::vector<PcieTlpType> order;
        uint32_t done = 0;
        f->SetObserver([&](const PcieTraceEvent& e) {
            if (e.event == "tx" && e.from == a)
            {
                order.push_back(e.type);
            }
        });
        f->Read(a, b, 1024, [&] { ++done; });
        f->Write(a, b, 16, [&] { ++done; });
        Simulator::Run();
        NS_TEST_ASSERT_MSG_EQ(done, 2, "Read and write drain without HOL deadlock");
        NS_TEST_ASSERT_MSG_EQ(order.size(), 3, "Two MRd and one MWr");
        NS_TEST_ASSERT_MSG_EQ(static_cast<int>(order[1]),
                              static_cast<int>(PcieTlpType::WRITE),
                              "MWr bypasses blocked MRd");
        f->Dispose();
        Simulator::Destroy();
    }
};

class PcieRdmaTest : public TestCase
{
  public:
    PcieRdmaTest()
        : TestCase("Detailed RDMA sends doorbell, WQE, DMA and CQ through PCIe")
    {
    }

    void DoRun() override
    {
        RdmaConfig c;
        c.pcieModel = "tlp";
        auto model = CreateObject<RdmaSubmissionModel>();
        model->Configure(c);
        auto fabric = model->GetPcieFabric();
        bool done = false;
        model->Submit(
            1,
            128,
            [model](uint64_t id) {
                model->SourceRead(128, [model, id] {
                    model->NotifyFirstWire(id);
                    model->NotifyOperationComplete(id);
                });
            },
            0,
            [&](bool success) { done = success; });
        Simulator::Run();
        NS_TEST_ASSERT_MSG_EQ(done, true, "CQ write permits WQE reclamation");
        const auto& bytes = fabric->GetStats().trafficBytes;
        NS_TEST_ASSERT_MSG_EQ(bytes[static_cast<size_t>(PcieTraffic::DOORBELL)],
                              8,
                              "Doorbell bytes");
        NS_TEST_ASSERT_MSG_EQ(bytes[static_cast<size_t>(PcieTraffic::WQE)], 64, "WQE bytes");
        NS_TEST_ASSERT_MSG_EQ(bytes[static_cast<size_t>(PcieTraffic::PAYLOAD)], 128, "DMA bytes");
        NS_TEST_ASSERT_MSG_EQ(bytes[static_cast<size_t>(PcieTraffic::CQ)], 64, "CQ bytes");
        model->Dispose();
        fabric->Dispose();
        Simulator::Destroy();
    }
};

class PcieCompletionOrderTest : public TestCase
{
  public:
    PcieCompletionOrderTest()
        : TestCase("PCIe completions obey Posted barriers and the RO attribute")
    {
    }

    void Check(bool relaxed)
    {
        auto f = CreateObject<PcieFabric>();
        PcieConfig c;
        c.credits[0] = 1;
        c.credits[1] = 16;
        f->Configure(c);
        auto a = f->AddEndpoint("a", {}, [](uint32_t) { return NanoSeconds(500); });
        auto b = f->AddEndpoint("b");
        f->AddLink(a, b);
        std::vector<PcieTlpType> order;
        uint32_t done = 0;
        f->SetObserver([&](const PcieTraceEvent& e) {
            if (e.event == "tx" && e.from == b)
            {
                order.push_back(e.type);
            }
        });
        f->Write(b, a, 512, [&] { ++done; });
        f->Read(a, b, 16, [&] { ++done; }, PcieTraffic::PAYLOAD, 0, relaxed);
        Simulator::Run();
        NS_TEST_ASSERT_MSG_EQ(done, 2, "No ordering/credit deadlock");
        NS_TEST_ASSERT_MSG_EQ(order.size(), 3, "Two MWr and one CplD");
        NS_TEST_ASSERT_MSG_EQ(
            static_cast<int>(order[1]),
            static_cast<int>(relaxed ? PcieTlpType::COMPLETION : PcieTlpType::WRITE),
            "Only an RO completion passes the blocked Posted request");
        f->Dispose();
        Simulator::Destroy();
    }

    void DoRun() override
    {
        Check(false);
        Check(true);
    }
};

class PcieFullDuplexTest : public TestCase
{
  public:
    PcieFullDuplexTest()
        : TestCase("PCIe opposite link directions serialize independently")
    {
    }

    void Measure(bool both, Time& duration)
    {
        auto f = CreateObject<PcieFabric>();
        PcieConfig c;
        c.credits[0] = 128;
        c.credits[1] = 8192;
        f->Configure(c);
        auto a = f->AddEndpoint("a"), b = f->AddEndpoint("b");
        f->AddLink(a, b);
        uint32_t done = 0;
        auto complete = [&] {
            ++done;
            duration = Simulator::Now();
        };
        f->Write(a, b, 8192, complete);
        if (both)
        {
            f->Write(b, a, 8192, complete);
        }
        Simulator::Run();
        NS_TEST_ASSERT_MSG_EQ(done, both ? 2 : 1, "All directions drain");
        f->Dispose();
        Simulator::Destroy();
    }

    void DoRun() override
    {
        Time one, two;
        Measure(false, one);
        Measure(true, two);
        NS_TEST_ASSERT_MSG_EQ(two.GetTimeStep() * 10 < one.GetTimeStep() * 14,
                              true,
                              "Two directions overlap, with DLLP contention accounted for");
    }
};

class PcieBoundaryTest : public TestCase
{
  public:
    PcieBoundaryTest()
        : TestCase("PCIe unaligned reads split at 4-KiB and RCB boundaries")
    {
    }

    void DoRun() override
    {
        auto f = CreateObject<PcieFabric>();
        PcieConfig c;
        c.mrrs = 4096;
        f->Configure(c);
        auto a = f->AddEndpoint("nic"), b = f->AddEndpoint("memory");
        f->AddLink(a, b);
        uint32_t requestBytes = 0, responseBytes = 0;
        bool valid = true, done = false;
        f->SetObserver([&](const PcieTraceEvent& e) {
            if (e.event != "tx")
            {
                return;
            }
            if (e.type == PcieTlpType::READ)
            {
                requestBytes += e.bytes;
                valid &= e.address / 4096 == (e.address + e.bytes - 1) / 4096;
            }
            if (e.type == PcieTlpType::COMPLETION)
            {
                responseBytes += e.bytes;
            }
        });
        f->Read(a, b, 8200, [&] { done = true; }, PcieTraffic::PAYLOAD, 4093);
        Simulator::Run();
        NS_TEST_ASSERT_MSG_EQ(done && valid,
                              true,
                              "Unaligned read reassembles with legal requests");
        NS_TEST_ASSERT_MSG_EQ(requestBytes, 8200, "No requested-byte padding charged to user");
        NS_TEST_ASSERT_MSG_EQ(responseBytes, 8200, "Exact valid bytes in completions");
        NS_TEST_ASSERT_MSG_EQ(f->GetStats().readRequests, 4, "3 + 4096 + 4096 + 5 B requests");
        f->Dispose();
        Simulator::Destroy();
    }
};

class PcieLifetimeTest : public TestCase
{
  public:
    PcieLifetimeTest()
        : TestCase("PCIe owner disposal suppresses callbacks while in-flight reads drain")
    {
    }

    void DoRun() override
    {
        RdmaConfig c;
        c.pcieModel = "tlp";
        c.initiator = "cpu";
        c.hostReadGbps = 128;
        auto model = CreateObject<RdmaSubmissionModel>();
        model->Configure(c);
        auto fabric = model->GetPcieFabric();
        bool executed = false, completed = false;
        model->Submit(
            1,
            128,
            [&](uint64_t) { executed = true; },
            0,
            [&](bool) { completed = true; });
        Simulator::Schedule(NanoSeconds(60), [model] { model->Dispose(); });
        Simulator::Run();
        NS_TEST_ASSERT_MSG_EQ(executed || completed, false, "Disposed owner is never called");
        NS_TEST_ASSERT_MSG_EQ(fabric->GetStats().readRequests, 1, "Descriptor read was in flight");
        NS_TEST_ASSERT_MSG_EQ(fabric->HasPending(), false, "Transport drains after owner disposal");
        fabric->Dispose();
        Simulator::Destroy();
    }
};

class PcieLegacySubtickTest : public TestCase
{
  public:
    PcieLegacySubtickTest()
        : TestCase("Legacy fast-memory accesses may round to zero service ticks")
    {
    }

    void DoRun() override
    {
        RdmaConfig c;
        c.hbmReadGbps = 8000;
        c.pcieReadGbps = 256;
        auto model = CreateObject<RdmaSubmissionModel>();
        model->Configure(c);
        bool done = false;
        model->Submit(
            1,
            1,
            [model](uint64_t id) {
                model->SourceRead(1, [model, id] { model->NotifyOperationComplete(id); });
            },
            0,
            [&](bool ok) { done = ok; });
        Simulator::Stop(MicroSeconds(1));
        Simulator::Run();
        NS_TEST_ASSERT_MSG_EQ(done, true, "Sub-tick memory service does not pin work at Time::Max");
        model->Dispose();
        Simulator::Destroy();
    }
};

class PcieRdmaAddressTest : public TestCase
{
  public:
    PcieRdmaAddressTest()
        : TestCase("RDMA DMA preserves address alignment and 4-KiB splits")
    {
    }

    void DoRun() override
    {
        RdmaConfig c;
        c.pcieModel = "tlp";
        auto model = CreateObject<RdmaSubmissionModel>();
        model->Configure(c);
        auto fabric = model->GetPcieFabric();
        uint32_t done = 0;
        model->SourceRead(8, [&] { ++done; }, 4093);
        model->DestinationWrite(8, [&] { ++done; }, 4093);
        Simulator::Run();
        NS_TEST_ASSERT_MSG_EQ(done, 2, "Both memory paths complete");
        NS_TEST_ASSERT_MSG_EQ(fabric->GetStats().readRequests, 2, "Read splits into 3 and 5 B");
        NS_TEST_ASSERT_MSG_EQ(fabric->GetStats().writeTlps,
                              2,
                              "Posted write splits into 3 and 5 B");
        model->Dispose();
        fabric->Dispose();
        Simulator::Destroy();
    }
};

class PcieSuite : public TestSuite
{
  public:
    PcieSuite()
        : TestSuite("gpu-cluster-pcie", Type::UNIT)
    {
        AddTestCase(new PcieHeaderTest, TestCase::Duration::QUICK);
        AddTestCase(new PcieCreditTest, TestCase::Duration::QUICK);
        AddTestCase(new PcieReadWindowTest, TestCase::Duration::QUICK);
        AddTestCase(new PciePathTest, TestCase::Duration::QUICK);
        AddTestCase(new PciePostedTest, TestCase::Duration::QUICK);
        AddTestCase(new PcieOrderingTest, TestCase::Duration::QUICK);
        AddTestCase(new PcieRdmaTest, TestCase::Duration::QUICK);
        AddTestCase(new PcieCompletionOrderTest, TestCase::Duration::QUICK);
        AddTestCase(new PcieFullDuplexTest, TestCase::Duration::QUICK);
        AddTestCase(new PcieBoundaryTest, TestCase::Duration::QUICK);
        AddTestCase(new PcieLifetimeTest, TestCase::Duration::QUICK);
        AddTestCase(new PcieLegacySubtickTest, TestCase::Duration::QUICK);
        AddTestCase(new PcieRdmaAddressTest, TestCase::Duration::QUICK);
    }
} g_pcieSuite;
