/* SPDX-License-Identifier: GPL-2.0-only */
#include "pcie-fabric.h"

#include "ns3/abort.h"
#include "ns3/simulator.h"
#include "ns3/trace-source-accessor.h"

#include <algorithm>
#include <deque>
#include <limits>
#include <map>
#include <queue>
#include <tuple>
#include <utility>
#include <vector>

namespace ns3
{
NS_OBJECT_ENSURE_REGISTERED(PcieFabric);
NS_OBJECT_ENSURE_REGISTERED(PcieTlpHeader);

namespace
{
constexpr uint32_t NO_PORT = UINT32_MAX;
constexpr uint64_t WINDOW = 1ull << 40;
// Non-FLIT performance accounting: sequence, LCRC and framing; no ECRC.
constexpr uint32_t TLP_LINK_BYTES = 8, DLLP_WIRE_BYTES = 8;

Time
AddTime(Time a, Time b)
{
    NS_ABORT_MSG_IF(a.IsStrictlyNegative() || b.IsStrictlyNegative(), "Negative PCIe latency");
    return b > Time::Max() - a ? Time::Max() : a + b;
}

uint32_t
CreditClass(PcieTlpType type)
{
    return type == PcieTlpType::WRITE ? 0 : (type == PcieTlpType::READ ? 2 : 4);
}

bool
CanPass(const PcieTlpHeader& later, const PcieTlpHeader& earlier)
{
    if (later.type == PcieTlpType::COMPLETION && earlier.type == PcieTlpType::COMPLETION)
    {
        return later.requester != earlier.requester || later.tag != earlier.tag;
    }
    if (earlier.type == PcieTlpType::WRITE)
    {
        return later.relaxed;
    }
    if (earlier.type == PcieTlpType::READ)
    {
        return later.type != PcieTlpType::READ || later.relaxed;
    }
    return true;
}

uint32_t
FirstBit(uint32_t mask)
{
    for (uint32_t i = 0; i < 4; ++i)
    {
        if (mask & (1u << i))
        {
            return i;
        }
    }
    return 0;
}

uint32_t
LastBit(uint32_t mask)
{
    for (uint32_t i = 4; i > 0; --i)
    {
        if (mask & (1u << (i - 1)))
        {
            return i - 1;
        }
    }
    return 0;
}
} // namespace

const char*
PcieTrafficName(PcieTraffic traffic)
{
    static const char* names[] = {"payload", "wqe", "doorbell", "cq", "d2h", "h2d"};
    return static_cast<size_t>(traffic) < static_cast<size_t>(PcieTraffic::COUNT)
               ? names[static_cast<size_t>(traffic)]
               : "unknown";
}

const char*
PcieTlpName(PcieTlpType type)
{
    return type == PcieTlpType::READ ? "MRd" : (type == PcieTlpType::WRITE ? "MWr" : "CplD");
}

void
PcieConfig::Validate() const
{
    auto powerOfTwo = [](uint32_t n) { return n && !(n & (n - 1)); };
    NS_ABORT_MSG_IF(generation < 3 || generation > 5, "pcieGeneration supports Gen3--5 non-FLIT");
    NS_ABORT_MSG_IF(!powerOfTwo(lanes) || lanes > 32, "pcieLanes must be 1,2,4,8,16,32");
    NS_ABORT_MSG_IF(topology != "switch" && topology != "root",
                    "pcieTopology must be switch or root");
    NS_ABORT_MSG_IF(!powerOfTwo(mps) || mps < 128 || mps > 4096,
                    "pcieMps must be a power of two in [128,4096]");
    NS_ABORT_MSG_IF(!powerOfTwo(mrrs) || mrrs < 128 || mrrs > 4096,
                    "pcieMrrs must be a power of two in [128,4096]");
    NS_ABORT_MSG_IF(rcb != 64 && rcb != 128, "pcieRcb must be 64 or 128");
    NS_ABORT_MSG_IF(tags == 0 || tags > 256, "pcieTags must be in [1,256] (8-bit tags)");
    NS_ABORT_MSG_IF(maxReadBytes < 4 || completionBufferBytes < 4,
                    "PCIe read-byte and completion-buffer limits must be at least 4 B");
    NS_ABORT_MSG_IF(queuePackets == 0 || queuePackets > 65536,
                    "pcieQueuePackets must be in [1,65536]");
    NS_ABORT_MSG_IF(credits[0] == 0 || credits[2] == 0 || credits[4] == 0 ||
                        credits[1] < mps / 16 || credits[5] < mps / 16,
                    "PCIe credits must hold a maximum-sized Posted/Completion TLP and an MRd");
    for (auto c : credits)
    {
        NS_ABORT_MSG_IF(c > (1u << 24), "PCIe credit capacity exceeds supported bounds");
    }
    NS_ABORT_MSG_IF(replayBufferBytes < mps + 16 + TLP_LINK_BYTES,
                    "pcieReplayBufferBytes must hold a maximum-sized TLP");
    NS_ABORT_MSG_IF(ackEvery == 0 || ackEvery > 65536, "pcieAckEvery must be in [1,65536]");
    for (auto t : {linkLatency, switchLatency, rootLatency, creditUpdate, ackDelay})
    {
        NS_ABORT_MSG_IF(t.IsStrictlyNegative(), "PCIe timings must be nonnegative");
    }
}

bool
PcieConfig::operator==(const PcieConfig& o) const
{
    return std::tie(generation,
                    lanes,
                    topology,
                    mps,
                    mrrs,
                    rcb,
                    tags,
                    maxReadBytes,
                    completionBufferBytes,
                    queuePackets,
                    credits,
                    replayBufferBytes,
                    ackEvery,
                    linkLatency,
                    switchLatency,
                    rootLatency,
                    creditUpdate,
                    ackDelay,
                    relaxedOrdering) == std::tie(o.generation,
                                                 o.lanes,
                                                 o.topology,
                                                 o.mps,
                                                 o.mrrs,
                                                 o.rcb,
                                                 o.tags,
                                                 o.maxReadBytes,
                                                 o.completionBufferBytes,
                                                 o.queuePackets,
                                                 o.credits,
                                                 o.replayBufferBytes,
                                                 o.ackEvery,
                                                 o.linkLatency,
                                                 o.switchLatency,
                                                 o.rootLatency,
                                                 o.creditUpdate,
                                                 o.ackDelay,
                                                 o.relaxedOrdering);
}

TypeId
PcieTlpHeader::GetTypeId()
{
    static TypeId tid = TypeId("ns3::PcieTlpHeader")
                            .SetParent<Header>()
                            .SetGroupName("GpuCluster")
                            .AddConstructor<PcieTlpHeader>();
    return tid;
}

TypeId
PcieTlpHeader::GetInstanceTypeId() const
{
    return GetTypeId();
}

uint32_t
PcieTlpHeader::GetSerializedSize() const
{
    return type == PcieTlpType::COMPLETION ? 12 : 16;
}

uint32_t
PcieTlpHeader::PayloadBytesOnWire() const
{
    return type == PcieTlpType::READ ? 0 : ((address % 4 + bytes + 3) / 4) * 4;
}

void
PcieTlpHeader::Serialize(Buffer::Iterator i) const
{
    uint32_t dwords = (address % 4 + bytes + 3) / 4;
    i.WriteHtonU32((static_cast<uint32_t>(type) << 24) | (relaxed ? 1u << 13 : 0) |
                   (dwords & 1023));
    if (type == PcieTlpType::COMPLETION)
    {
        i.WriteHtonU32((static_cast<uint32_t>(completer) << 16) | (byteCount & 4095));
        i.WriteHtonU32((static_cast<uint32_t>(requester) << 16) |
                       (static_cast<uint32_t>(tag) << 8) | (address & 127));
    }
    else
    {
        uint32_t first = (15u << (address % 4)) & 15;
        uint32_t last = (1u << ((address + bytes - 1) % 4 + 1)) - 1;
        if (dwords == 1)
        {
            first &= last;
            last = 0;
        }
        i.WriteHtonU32((static_cast<uint32_t>(requester) << 16) |
                       (static_cast<uint32_t>(tag) << 8) | (last << 4) | first);
        i.WriteHtonU32(address >> 32);
        i.WriteHtonU32(static_cast<uint32_t>(address) & ~3u);
    }
}

uint32_t
PcieTlpHeader::Deserialize(Buffer::Iterator i)
{
    uint32_t dw0 = i.ReadNtohU32(), dw1 = i.ReadNtohU32(), dw2 = i.ReadNtohU32();
    type = static_cast<PcieTlpType>(dw0 >> 24);
    relaxed = (dw0 >> 13) & 1;
    uint32_t words = dw0 & 1023;
    if (!words)
    {
        words = 1024;
    }
    if (type == PcieTlpType::COMPLETION)
    {
        completer = dw1 >> 16;
        byteCount = dw1 & 4095;
        if (!byteCount)
        {
            byteCount = 4096;
        }
        requester = dw2 >> 16;
        tag = (dw2 >> 8) & 255;
        address = dw2 & 127;
        bytes = std::min<uint32_t>(byteCount, words * 4 - address % 4);
    }
    else
    {
        requester = dw1 >> 16;
        tag = (dw1 >> 8) & 255;
        uint32_t first = dw1 & 15, last = (dw1 >> 4) & 15;
        address = (static_cast<uint64_t>(dw2) << 32) | (i.ReadNtohU32() & ~3u);
        address += FirstBit(first);
        bytes = words == 1 ? LastBit(first) - FirstBit(first) + 1
                           : (words - 1) * 4 + LastBit(last) + 1 - FirstBit(first);
    }
    return GetSerializedSize();
}

void
PcieTlpHeader::Print(std::ostream& os) const
{
    os << PcieTlpName(type) << " requester=" << requester << " tag=" << unsigned(tag)
       << " address=" << address << " bytes=" << bytes;
}

struct PcieFabric::Impl
{
    struct Frame
    {
        Ptr<Packet> packet;
        PcieTlpHeader header;
        uint64_t transaction{0}, offset{0}, order{0};
        PcieTraffic traffic;
        uint16_t destination{0};
        uint32_t ingress{NO_PORT}, domain{NO_PORT};
        Time ready;
    };

    struct Control
    {
        bool ack{false};
        uint32_t forPort{0};
        uint64_t sequence{0};
        std::array<uint32_t, 6> credits{};
    };

    struct Port
    {
        uint16_t from{0}, to{0};
        uint32_t reverse{0}, nextClass{0};
        std::deque<std::shared_ptr<Frame>> queue;
        std::deque<Control> control;
        std::array<uint32_t, 6> available{}, pendingReturn{};
        uint64_t nextSequence{1}, receivedSequence{0}, ackCount{0}, replayBytes{0};
        std::deque<std::pair<uint64_t, uint32_t>> replay;
        EventId fcEvent, ackEvent, wake;
        bool busy{false}, creditBlocked{false}, queueBlocked{false}, replayBlocked{false};
        bool orderBlocked{false};
    };

    struct ReadState
    {
        uint64_t transaction, offset;
        uint32_t bytes, received{0};
        std::map<uint64_t, uint32_t> pieces;
    };

    struct Response
    {
        std::shared_ptr<Frame> request;
        uint32_t next{0};
    };

    struct Node
    {
        std::string name;
        MemoryService read, write;
        Time forwarding;
        bool endpoint{true}, pumping{false};
        std::vector<uint32_t> ports;
        // Independent injection queues let Posted requests pass a blocked MRd
        // without scanning the entire application backlog on every link event.
        std::array<std::deque<uint64_t>, 2> transfers;
        std::deque<Response> responses;
        std::deque<std::shared_ptr<Frame>> waiting;
        std::map<uint8_t, ReadState> reads;
        uint64_t readBytes{0};
    };

    struct Transfer
    {
        uint64_t id, address, bytes, next{0}, pending{0};
        uint16_t requester, target;
        bool read, relaxed, tagBlocked{false};
        PcieTraffic traffic;
        Ready complete;
    };

    explicit Impl(PcieFabric* owner)
        : owner(owner)
    {
    }

    PcieFabric* owner;
    PcieConfig config;
    PcieStats stats;
    Observer observer;
    bool disposed{false};
    uint64_t nextId{1}, nextOrder{1};
    std::vector<Node> nodes;
    std::vector<Port> ports;
    std::map<std::pair<uint16_t, uint16_t>, uint32_t> routes;
    std::map<uint64_t, std::shared_ptr<Transfer>> transfers;
    std::vector<EventId> events;

    EventId Schedule(Time delay, Ready ready)
    {
        NS_ABORT_MSG_IF(delay.IsStrictlyNegative(), "Negative PCIe service time");
        if (events.size() % 1024 == 0)
        {
            events.erase(std::remove_if(events.begin(),
                                        events.end(),
                                        [](const auto& e) { return e.IsExpired(); }),
                         events.end());
        }
        delay = std::min(delay, Time::Max() - Simulator::Now());
        events.push_back(Simulator::Schedule(delay, [this, ready = std::move(ready)] {
            if (!disposed)
            {
                ready();
            }
        }));
        return events.back();
    }

    Time WireTime(uint32_t bytes) const
    {
        double gt = config.generation == 3 ? 8.0 : (config.generation == 4 ? 16.0 : 32.0);
        return std::max(TimeStep(1), Seconds(bytes * 8.0 / (gt * config.lanes * 1e9 * 128 / 130)));
    }

    void Record(const char* event,
                uint32_t port,
                const std::shared_ptr<Frame>& frame = {},
                uint32_t wireBytes = 0)
    {
        if (!observer)
        {
            return;
        }
        PcieTraceEvent e;
        e.time = Simulator::Now();
        e.event = event;
        e.wireBytes = wireBytes;
        if (port != NO_PORT)
        {
            e.from = ports[port].from;
            e.to = ports[port].to;
            e.credits = ports[port].available;
        }
        if (frame)
        {
            e.transaction = frame->transaction;
            e.address = frame->header.address;
            e.type = frame->header.type;
            e.traffic = frame->traffic;
            e.bytes = frame->header.bytes;
            e.tag = frame->header.tag;
        }
        observer(e);
    }

    uint32_t Route(uint16_t from, uint16_t to)
    {
        auto key = std::make_pair(from, to);
        auto cached = routes.find(key);
        if (cached != routes.end())
        {
            return cached->second;
        }
        std::queue<uint16_t> q;
        std::vector<uint32_t> first(nodes.size(), NO_PORT);
        q.push(from);
        while (!q.empty())
        {
            uint16_t at = q.front();
            q.pop();
            for (auto p : nodes[at].ports)
            {
                uint16_t next = ports[p].to;
                if (next == from || first[next] != NO_PORT)
                {
                    continue;
                }
                first[next] = at == from ? p : first[at];
                if (next == to)
                {
                    routes[key] = first[next];
                    return first[next];
                }
                q.push(next);
            }
        }
        NS_ABORT_MSG("No PCIe route from " << from << " to " << to);
        return NO_PORT;
    }

    uint16_t Resolve(uint64_t address) const
    {
        uint64_t number = address / WINDOW;
        NS_ABORT_MSG_IF(number == 0 || number > nodes.size() || !nodes[number - 1].endpoint,
                        "Unmapped PCIe memory address " << address);
        return number - 1;
    }

    std::shared_ptr<Frame> MakeFrame(PcieTlpHeader header,
                                     const Transfer& transfer,
                                     uint64_t offset,
                                     uint16_t destination)
    {
        auto f = std::make_shared<Frame>();
        f->header = header;
        f->transaction = transfer.id;
        f->offset = offset;
        f->traffic = transfer.traffic;
        f->destination = destination;
        f->packet = Create<Packet>(header.PayloadBytesOnWire());
        f->packet->AddHeader(header);
        f->ready = Simulator::Now();
        f->order = nextOrder++;
        return f;
    }

    bool Space(uint32_t p)
    {
        if (ports[p].queue.size() < config.queuePackets)
        {
            return true;
        }
        if (!ports[p].queueBlocked)
        {
            ports[p].queueBlocked = true;
            ++stats.queueStalls;
            Record("queue_stall", p);
        }
        return false;
    }

    void Enqueue(uint32_t p, std::shared_ptr<Frame> f)
    {
        ports[p].queue.push_back(std::move(f));
        stats.peakQueuePackets = std::max<uint64_t>(stats.peakQueuePackets, ports[p].queue.size());
    }

    void ReturnCredit(const std::shared_ptr<Frame>& f)
    {
        if (f->ingress == NO_PORT)
        {
            return;
        }
        uint32_t p = f->ingress, c = CreditClass(f->header.type);
        f->ingress = NO_PORT;
        ++ports[p].pendingReturn[c];
        ports[p].pendingReturn[c + 1] += (f->header.PayloadBytesOnWire() + 15) / 16;
        if (!ports[p].fcEvent.IsPending())
        {
            ports[p].fcEvent = Schedule(config.creditUpdate, [this, p] {
                if (disposed)
                {
                    return;
                }
                for (uint32_t c = 0; c < 6; c += 2)
                {
                    if (!ports[p].pendingReturn[c] && !ports[p].pendingReturn[c + 1])
                    {
                        continue;
                    }
                    Control fc;
                    fc.forPort = p;
                    fc.credits[c] = ports[p].pendingReturn[c];
                    fc.credits[c + 1] = ports[p].pendingReturn[c + 1];
                    ports[p].pendingReturn[c] = ports[p].pendingReturn[c + 1] = 0;
                    ports[ports[p].reverse].control.push_back(fc);
                }
                PumpPort(ports[p].reverse);
            });
        }
    }

    void SendAck(uint32_t p)
    {
        if (ports[p].ackEvent.IsPending())
        {
            Simulator::Cancel(ports[p].ackEvent);
        }
        if (!ports[p].ackCount)
        {
            return;
        }
        Control ack;
        ack.ack = true;
        ack.forPort = p;
        ack.sequence = ports[p].receivedSequence;
        ports[p].ackCount = 0;
        ports[ports[p].reverse].control.push_back(ack);
        PumpPort(ports[p].reverse);
    }

    void AckReceivedTlp(uint32_t p, uint64_t sequence)
    {
        ports[p].receivedSequence = sequence;
        if (++ports[p].ackCount >= config.ackEvery)
        {
            SendAck(p);
            return;
        }
        if (!ports[p].ackEvent.IsPending())
        {
            ports[p].ackEvent = Schedule(config.ackDelay, [this, p] {
                if (!disposed)
                {
                    SendAck(p);
                }
            });
        }
    }

    bool Ordered(uint16_t node, const std::shared_ptr<Frame>& candidate) const
    {
        auto blocks = [&candidate](const std::shared_ptr<Frame>& earlier) {
            return earlier->order < candidate->order && earlier->domain == candidate->domain &&
                   !CanPass(candidate->header, earlier->header);
        };
        for (auto p : nodes[node].ports)
        {
            for (const auto& f : ports[p].queue)
            {
                if (blocks(f))
                {
                    return false;
                }
            }
        }
        for (const auto& f : nodes[node].waiting)
        {
            if (blocks(f))
            {
                return false;
            }
        }
        return true;
    }

    void PumpPort(uint32_t p)
    {
        if (disposed || ports[p].busy)
        {
            return;
        }
        if (!ports[p].control.empty())
        {
            auto control = ports[p].control.front();
            ports[p].control.pop_front();
            ports[p].busy = true;
            Time service = WireTime(DLLP_WIRE_BYTES);
            stats.wireBytes += DLLP_WIRE_BYTES;
            Record(control.ack ? "ack_tx" : "update_fc_tx", p, {}, DLLP_WIRE_BYTES);
            Schedule(service, [this, p] {
                ports[p].busy = false;
                PumpNode(ports[p].from);
            });
            Schedule(AddTime(service, config.linkLatency), [this, control] {
                auto& target = ports[control.forPort];
                if (control.ack)
                {
                    ++stats.linkAcks;
                    while (!target.replay.empty() &&
                           target.replay.front().first <= control.sequence)
                    {
                        target.replayBytes -= target.replay.front().second;
                        target.replay.pop_front();
                    }
                    target.replayBlocked = false;
                    Record("link_ack", control.forPort);
                }
                else
                {
                    ++stats.flowUpdates;
                    for (size_t c = 0; c < 6; ++c)
                    {
                        target.available[c] += control.credits[c];
                        NS_ABORT_MSG_IF(target.available[c] > config.credits[c],
                                        "PCIe credit accounting overflow");
                    }
                    target.creditBlocked = false;
                    Record("credit_update", control.forPort);
                }
                PumpPort(control.forPort);
            });
            return;
        }
        size_t selected = ports[p].queue.size();
        bool blockedCredit = false, blockedReplay = false, blockedOrder = false;
        Time earliest = Time::Max();
        for (uint32_t turn = 0; turn < 3 && selected == ports[p].queue.size(); ++turn)
        {
            uint32_t category = ((ports[p].nextClass + turn) % 3) * 2;
            for (size_t i = 0; i < ports[p].queue.size(); ++i)
            {
                auto f = ports[p].queue[i];
                if (CreditClass(f->header.type) != category)
                {
                    continue;
                }
                if (f->ready > Simulator::Now())
                {
                    earliest = std::min(earliest, f->ready);
                    continue;
                }
                if (!Ordered(ports[p].from, f))
                {
                    blockedOrder = true;
                    continue;
                }
                uint32_t data = (f->header.PayloadBytesOnWire() + 15) / 16;
                if (!ports[p].available[category] || ports[p].available[category + 1] < data)
                {
                    blockedCredit = true;
                    continue;
                }
                if (f->packet->GetSize() + TLP_LINK_BYTES >
                    config.replayBufferBytes - ports[p].replayBytes)
                {
                    blockedReplay = true;
                    continue;
                }
                selected = i;
                break;
            }
        }
        auto stall = [&](bool blocked, bool& state, uint64_t& count, const char* name) {
            if (blocked && !state)
            {
                ++count;
                Record(name, p);
            }
            state = blocked;
        };
        stall(blockedCredit, ports[p].creditBlocked, stats.creditStalls, "credit_stall");
        stall(blockedReplay, ports[p].replayBlocked, stats.replayStalls, "replay_stall");
        stall(blockedOrder, ports[p].orderBlocked, stats.orderingStalls, "ordering_stall");
        if (selected == ports[p].queue.size())
        {
            if (earliest != Time::Max())
            {
                if (ports[p].wake.IsPending())
                {
                    Simulator::Cancel(ports[p].wake);
                }
                ports[p].wake = Schedule(earliest - Simulator::Now(), [this, p] {
                    if (!disposed)
                    {
                        PumpNode(ports[p].from);
                    }
                });
            }
            return;
        }
        auto f = ports[p].queue[selected];
        ports[p].queue.erase(ports[p].queue.begin() + selected);
        ports[p].queueBlocked = false;
        ReturnCredit(f);
        uint32_t category = CreditClass(f->header.type);
        --ports[p].available[category];
        ports[p].available[category + 1] -= (f->header.PayloadBytesOnWire() + 15) / 16;
        ports[p].nextClass = (category / 2 + 1) % 3;
        ports[p].busy = true;
        f->ingress = p;
        uint32_t wire = f->packet->GetSize() + TLP_LINK_BYTES;
        uint64_t seq = ports[p].nextSequence++;
        ports[p].replay.emplace_back(seq, wire);
        ports[p].replayBytes += wire;
        stats.peakReplayBytes = std::max(stats.peakReplayBytes, ports[p].replayBytes);
        ++stats.tlpHops;
        stats.wireBytes += wire;
        Time service = WireTime(wire);
        Record("tx", p, f, wire);
        owner->m_tlpTx(f->packet, ports[p].from, ports[p].to);
        Schedule(service, [this, p] {
            ports[p].busy = false;
            PumpNode(ports[p].from);
        });
        Schedule(AddTime(service, config.linkLatency), [this, p, f, seq] {
            Record("rx", p, f);
            owner->m_tlpRx(f->packet, ports[p].from, ports[p].to);
            AckReceivedTlp(p, seq);
            Receive(ports[p].to, f);
        });
    }

    void Finish(uint64_t id)
    {
        auto it = transfers.find(id);
        if (it == transfers.end() || it->second->next != it->second->bytes || it->second->pending)
        {
            return;
        }
        auto callback = std::move(it->second->complete);
        stats.lastCompletionNs = Simulator::Now().GetNanoSeconds();
        transfers.erase(it);
        if (callback)
        {
            Schedule(Time(0), std::move(callback));
        }
    }

    void Receive(uint16_t at, std::shared_ptr<Frame> f)
    {
        if (at != f->destination)
        {
            f->domain = f->ingress;
            f->order = nextOrder++;
            f->ready = AddTime(Simulator::Now(), nodes[at].forwarding);
            nodes[at].waiting.push_back(std::move(f));
            PumpNode(at);
            return;
        }
        if (f->header.type == PcieTlpType::READ)
        {
            ReturnCredit(f);
            Time service = nodes[at].read ? nodes[at].read(f->header.bytes) : Time(0);
            Schedule(service, [this, at, f] {
                nodes[at].responses.push_back({f});
                PumpNode(at);
            });
        }
        else if (f->header.type == PcieTlpType::WRITE)
        {
            Time service = nodes[at].write ? nodes[at].write(f->header.bytes) : Time(0);
            Schedule(service, [this, f] {
                ReturnCredit(f);
                auto it = transfers.find(f->transaction);
                NS_ABORT_MSG_IF(it == transfers.end() || !it->second->pending,
                                "Unknown PCIe write");
                --it->second->pending;
                Record("write_complete", NO_PORT, f);
                Finish(f->transaction);
            });
        }
        else
        {
            Time service = nodes[at].write ? nodes[at].write(f->header.bytes) : Time(0);
            Schedule(service, [this, at, f] {
                auto found = nodes[at].reads.find(f->header.tag);
                NS_ABORT_MSG_IF(found == nodes[at].reads.end(), "PCIe completion has no live tag");
                auto& r = found->second;
                NS_ABORT_MSG_IF(r.transaction != f->transaction || f->offset < r.offset ||
                                    f->offset - r.offset >= r.bytes ||
                                    f->header.bytes > r.bytes - (f->offset - r.offset),
                                "PCIe completion does not match its read request");
                auto next = r.pieces.lower_bound(f->offset);
                NS_ABORT_MSG_IF(
                    (next != r.pieces.end() && f->offset + f->header.bytes > next->first) ||
                        (next != r.pieces.begin() &&
                         std::prev(next)->first + std::prev(next)->second > f->offset),
                    "Overlapping PCIe completion");
                r.pieces.emplace(f->offset, f->header.bytes);
                r.received += f->header.bytes;
                ReturnCredit(f);
                if (r.received == r.bytes)
                {
                    uint64_t id = r.transaction;
                    nodes[at].readBytes -= r.bytes;
                    nodes[at].reads.erase(found);
                    auto op = transfers.find(id);
                    NS_ABORT_MSG_IF(op == transfers.end() || !op->second->pending,
                                    "Unknown PCIe read");
                    --op->second->pending;
                    Record("read_complete", NO_PORT, f);
                    Finish(id);
                    PumpNode(at);
                }
            });
        }
    }

    bool IssueResponse(uint16_t at)
    {
        if (nodes[at].responses.empty())
        {
            return false;
        }
        auto& response = nodes[at].responses.front();
        auto request = response.request;
        uint32_t p = Route(at, request->header.requester);
        if (!Space(p))
        {
            return false;
        }
        auto op = transfers.find(request->transaction);
        NS_ABORT_MSG_IF(op == transfers.end(), "PCIe response lost its transaction");
        uint64_t address = request->header.address + response.next;
        uint32_t remain = request->header.bytes - response.next;
        uint32_t bytes = std::min(config.mps - static_cast<uint32_t>(address % 4), remain);
        // Non-final completions end on an RCB boundary; multiple RCBs may fit in one CplD.
        if (bytes < remain)
        {
            bytes -= (address + bytes) % config.rcb;
        }
        NS_ABORT_MSG_IF(bytes == 0, "PCIe completion split made no progress");
        auto h = request->header;
        h.type = PcieTlpType::COMPLETION;
        h.completer = at;
        h.address = address;
        h.bytes = bytes;
        h.byteCount = remain;
        auto f = MakeFrame(h, *op->second, request->offset + response.next, h.requester);
        Enqueue(p, f);
        ++stats.completionTlps;
        response.next += bytes;
        if (response.next == request->header.bytes)
        {
            nodes[at].responses.pop_front();
        }
        return true;
    }

    bool IssueTransfer(uint16_t at, const std::shared_ptr<Transfer>& op)
    {
        if (op->next == op->bytes)
        {
            return false;
        }
        uint32_t p = Route(at, Resolve(op->address));
        if (!Space(p))
        {
            return false;
        }
        uint64_t address = op->address + op->next;
        uint64_t limit = op->read ? config.mrrs : config.mps;
        if (op->read)
        {
            limit = std::min<uint64_t>({limit, config.maxReadBytes, config.completionBufferBytes});
        }
        uint32_t bytes =
            std::min<uint64_t>({limit - address % 4, op->bytes - op->next, 4096 - address % 4096});
        uint8_t tag = 0;
        if (op->read)
        {
            auto& n = nodes[at];
            if (n.reads.size() == config.tags || bytes > config.maxReadBytes - n.readBytes ||
                bytes > config.completionBufferBytes - n.readBytes)
            {
                if (!op->tagBlocked)
                {
                    ++stats.tagStalls;
                    Record("read_window_stall", p);
                }
                op->tagBlocked = true;
                return false;
            }
            uint32_t number = 0;
            while (n.reads.count(number))
            {
                ++number;
            }
            tag = number;
            n.reads.emplace(tag, ReadState{op->id, op->next, bytes});
            n.readBytes += bytes;
            stats.peakOutstandingReads =
                std::max<uint64_t>(stats.peakOutstandingReads, n.reads.size());
            stats.peakOutstandingReadBytes = std::max(stats.peakOutstandingReadBytes, n.readBytes);
        }
        op->tagBlocked = false;
        PcieTlpHeader h;
        h.type = op->read ? PcieTlpType::READ : PcieTlpType::WRITE;
        h.address = address;
        h.bytes = bytes;
        h.requester = at;
        h.tag = tag;
        h.relaxed = op->relaxed;
        auto f = MakeFrame(h, *op, op->next, op->target);
        if (!op->read && op->traffic != PcieTraffic::DOORBELL && nodes[at].read)
        {
            f->ready = AddTime(f->ready, nodes[at].read(bytes));
        }
        Enqueue(p, f);
        op->next += bytes;
        ++op->pending;
        if (op->read)
        {
            ++stats.readRequests;
        }
        else
        {
            ++stats.writeTlps;
        }
        return true;
    }

    void PumpNode(uint16_t at)
    {
        if (disposed || nodes[at].pumping)
        {
            return;
        }
        nodes[at].pumping = true;
        bool progress;
        do
        {
            progress = false;
            for (auto it = nodes[at].waiting.begin(); it != nodes[at].waiting.end();)
            {
                auto f = *it;
                uint32_t p = Route(at, f->destination);
                if (!Space(p))
                {
                    ++it;
                    continue;
                }
                Enqueue(p, f);
                it = nodes[at].waiting.erase(it);
                progress = true;
            }
            progress |= IssueResponse(at);
            for (auto& pending : nodes[at].transfers)
            {
                if (pending.empty())
                {
                    continue;
                }
                auto id = pending.front();
                auto it = transfers.find(id);
                if (it != transfers.end())
                {
                    progress |= IssueTransfer(at, it->second);
                }
                if (it == transfers.end() || it->second->next == it->second->bytes)
                {
                    pending.pop_front();
                }
            }
            for (auto p : nodes[at].ports)
            {
                PumpPort(p);
            }
        } while (progress && !disposed);
        nodes[at].pumping = false;
    }

    uint64_t Submit(bool read,
                    uint16_t requester,
                    uint16_t target,
                    uint64_t bytes,
                    Ready ready,
                    PcieTraffic traffic,
                    uint64_t offset,
                    bool relaxed)
    {
        NS_ABORT_MSG_IF(disposed || requester >= nodes.size() || target >= nodes.size() ||
                            !nodes[requester].endpoint || !nodes[target].endpoint ||
                            requester == target,
                        "PCIe transfer needs two distinct live endpoints");
        NS_ABORT_MSG_IF(!bytes || offset >= WINDOW || bytes > WINDOW - offset,
                        "PCIe transfer exceeds the endpoint memory window");
        NS_ABORT_MSG_IF(static_cast<size_t>(traffic) >= static_cast<size_t>(PcieTraffic::COUNT),
                        "Unknown PCIe traffic class");
        auto op = std::make_shared<Transfer>();
        op->id = nextId++;
        op->requester = requester;
        op->target = target;
        op->address = (static_cast<uint64_t>(target) + 1) * WINDOW + offset;
        op->bytes = bytes;
        op->read = read;
        op->relaxed = relaxed || config.relaxedOrdering;
        op->traffic = traffic;
        op->complete = std::move(ready);
        transfers.emplace(op->id, op);
        nodes[requester].transfers[read ? 1 : 0].push_back(op->id);
        if (read)
        {
            ++stats.reads;
        }
        else
        {
            ++stats.writes;
        }
        stats.trafficBytes[static_cast<size_t>(traffic)] += bytes;
        PumpNode(requester);
        return op->id;
    }

    void Dispose()
    {
        disposed = true;
        for (auto e : events)
        {
            if (e.IsPending())
            {
                Simulator::Cancel(e);
            }
        }
        for (auto& p : ports)
        {
            for (auto e : {p.fcEvent, p.ackEvent, p.wake})
            {
                if (e.IsPending())
                {
                    Simulator::Cancel(e);
                }
            }
        }
        observer = {};
        for (auto& [id, op] : transfers)
        {
            op->complete = {};
        }
    }
};

TypeId
PcieFabric::GetTypeId()
{
    static TypeId tid = TypeId("ns3::PcieFabric")
                            .SetParent<Object>()
                            .SetGroupName("GpuCluster")
                            .AddConstructor<PcieFabric>()
                            .AddTraceSource("TlpTx",
                                            "TLP sent on one PCIe hop (without link framing).",
                                            MakeTraceSourceAccessor(&PcieFabric::m_tlpTx),
                                            "ns3::PcieFabric::TlpTraceCallback")
                            .AddTraceSource("TlpRx",
                                            "TLP received on one PCIe hop.",
                                            MakeTraceSourceAccessor(&PcieFabric::m_tlpRx),
                                            "ns3::PcieFabric::TlpTraceCallback");
    return tid;
}

PcieFabric::PcieFabric()
    : m(std::make_unique<Impl>(this))
{
}

PcieFabric::~PcieFabric()
{
    m->Dispose();
}

void
PcieFabric::Configure(const PcieConfig& config)
{
    NS_ABORT_MSG_IF(!m->nodes.empty() || m->disposed,
                    "Configure PCIe before constructing topology");
    config.Validate();
    m->config = config;
}

const PcieConfig&
PcieFabric::GetConfig() const
{
    return m->config;
}

uint16_t
PcieFabric::AddEndpoint(const std::string& name, MemoryService read, MemoryService write)
{
    NS_ABORT_MSG_IF(m->disposed || m->stats.reads || m->stats.writes || m->nodes.size() >= 65535,
                    "Construct PCIe topology before traffic");
    Impl::Node node;
    node.name = name;
    node.read = std::move(read);
    node.write = std::move(write);
    node.forwarding = m->config.switchLatency;
    m->nodes.push_back(std::move(node));
    return m->nodes.size() - 1;
}

uint16_t
PcieFabric::AddRootComplex(const std::string& name, MemoryService read, MemoryService write)
{
    uint16_t id = AddEndpoint(name, std::move(read), std::move(write));
    m->nodes[id].forwarding = m->config.rootLatency;
    return id;
}

uint16_t
PcieFabric::AddSwitch(const std::string& name)
{
    uint16_t id = AddEndpoint(name);
    m->nodes[id].endpoint = false;
    return id;
}

void
PcieFabric::AddLink(uint16_t a, uint16_t b)
{
    NS_ABORT_MSG_IF(m->disposed || a >= m->nodes.size() || b >= m->nodes.size() || a == b ||
                        m->stats.reads || m->stats.writes,
                    "Invalid PCIe link/topology mutation");
    for (auto p : m->nodes[a].ports)
    {
        NS_ABORT_MSG_IF(m->ports[p].to == b, "Duplicate PCIe link");
    }
    uint32_t index = m->ports.size();
    Impl::Port ab, ba;
    ab.from = a;
    ab.to = b;
    ab.reverse = index + 1;
    ab.available = m->config.credits;
    ba.from = b;
    ba.to = a;
    ba.reverse = index;
    ba.available = m->config.credits;
    m->ports.push_back(std::move(ab));
    m->ports.push_back(std::move(ba));
    m->nodes[a].ports.push_back(index);
    m->nodes[b].ports.push_back(index + 1);
    m->routes.clear();
}

uint64_t
PcieFabric::GetAddress(uint16_t endpoint, uint64_t offset) const
{
    NS_ABORT_MSG_IF(endpoint >= m->nodes.size() || !m->nodes[endpoint].endpoint || offset >= WINDOW,
                    "Invalid PCIe address window");
    return (static_cast<uint64_t>(endpoint) + 1) * WINDOW + offset;
}

uint64_t
PcieFabric::Read(uint16_t requester,
                 uint16_t target,
                 uint64_t bytes,
                 Ready ready,
                 PcieTraffic traffic,
                 uint64_t offset,
                 bool relaxed)
{
    return m->Submit(true, requester, target, bytes, std::move(ready), traffic, offset, relaxed);
}

uint64_t
PcieFabric::Write(uint16_t requester,
                  uint16_t target,
                  uint64_t bytes,
                  Ready ready,
                  PcieTraffic traffic,
                  uint64_t offset,
                  bool relaxed)
{
    return m->Submit(false, requester, target, bytes, std::move(ready), traffic, offset, relaxed);
}

const PcieStats&
PcieFabric::GetStats() const
{
    return m->stats;
}

void
PcieFabric::SetObserver(Observer observer)
{
    m->observer = std::move(observer);
}

bool
PcieFabric::HasPending() const
{
    if (m->disposed)
    {
        return false;
    }
    if (!m->transfers.empty())
    {
        return true;
    }
    for (const auto& p : m->ports)
    {
        if (p.busy || !p.queue.empty() || !p.control.empty() || p.fcEvent.IsPending() ||
            p.ackEvent.IsPending())
        {
            return true;
        }
    }
    return false;
}

void
PcieFabric::DoDispose()
{
    m->Dispose();
    Object::DoDispose();
}
} // namespace ns3
