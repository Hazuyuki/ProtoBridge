/* SPDX-License-Identifier: GPL-2.0-only */
#include "rdma-operation-engine.h"

#include "ns3/abort.h"
#include "ns3/simulator.h"

#include <algorithm>
#include <limits>

namespace ns3
{
NS_OBJECT_ENSURE_REGISTERED(RdmaOperationEngine);

namespace
{
auto
AckKey(const FabricHeader& h)
{
    return std::make_tuple(h.GetSourceRank(),
                           h.GetVirtualChannel(),
                           h.GetFlowId(),
                           h.GetSequenceNumber());
}

auto
TxKey(const FabricHeader& h)
{
    return std::make_tuple(h.GetDestRank(),
                           h.GetVirtualChannel(),
                           h.GetFlowId(),
                           h.GetSequenceNumber());
}

bool
Compare(uint64_t actual, RdmaComparison comparison, uint64_t expected)
{
    switch (comparison)
    {
    case RdmaComparison::EQ:
        return actual == expected;
    case RdmaComparison::NE:
        return actual != expected;
    case RdmaComparison::GT:
        return actual > expected;
    case RdmaComparison::GE:
        return actual >= expected;
    case RdmaComparison::LT:
        return actual < expected;
    case RdmaComparison::LE:
        return actual <= expected;
    }
    return false;
}

std::vector<uint8_t>
WordBytes(uint64_t value)
{
    std::vector<uint8_t> bytes(8);
    for (uint32_t i = 0; i < 8; ++i)
    {
        bytes[i] = (value >> (8 * i)) & 255;
    }
    return bytes;
}
} // namespace

TypeId
RdmaOperationEngine::GetTypeId()
{
    static TypeId tid = TypeId("ns3::RdmaOperationEngine")
                            .SetParent<Object>()
                            .SetGroupName("GpuCluster")
                            .AddConstructor<RdmaOperationEngine>();
    return tid;
}

void
RdmaOperationEngine::Configure(uint16_t rank,
                               Ptr<RdmaSubmissionModel> model,
                               Transmit transmit,
                               Ack ack,
                               HoldAck hold)
{
    NS_ABORT_MSG_IF(!model || !transmit || !ack || !hold || HasPending(),
                    "Configure idle RDMA engine");
    m_rank = rank;
    m_model = model;
    m_transmit = std::move(transmit);
    m_ack = std::move(ack);
    m_hold = std::move(hold);
}

void
RdmaOperationEngine::Schedule(Time delay, std::function<void()> action)
{
    // A large direct transfer can schedule thousands of packets at once.
    // Periodic collection avoids scanning every live event for every packet.
    if (m_events.size() % 1024 == 0)
    {
        m_events.erase(std::remove_if(m_events.begin(),
                                      m_events.end(),
                                      [](const auto& event) { return event.IsExpired(); }),
                       m_events.end());
    }
    m_events.push_back(Simulator::Schedule(delay, [this, action = std::move(action)] {
        if (!m_disposed)
        {
            action();
        }
    }));
}

const RdmaOperationStats&
RdmaOperationEngine::GetStats() const
{
    return m_stats;
}

void
RdmaOperationEngine::SetPayloadLimit(std::function<uint32_t(uint16_t)> limit)
{
    m_payloadLimit = std::move(limit);
}

bool
RdmaOperationEngine::HasPending() const
{
    return !m_sources.empty() || !m_incoming.empty() || !m_responders.empty() ||
           !m_responderWaiting.empty();
}

bool
RdmaOperationEngine::InRegion(uint32_t key, uint64_t address, uint64_t bytes) const
{
    auto found = m_regions.find(key);
    return found != m_regions.end() && address >= found->second.base &&
           address - found->second.base <= found->second.bytes &&
           bytes <= found->second.bytes - (address - found->second.base);
}

bool
RdmaOperationEngine::InAnyRegion(uint64_t address, uint64_t bytes) const
{
    for (const auto& [key, region] : m_regions)
    {
        if (InRegion(key, address, bytes))
        {
            return true;
        }
    }
    return false;
}

void
RdmaOperationEngine::RegisterMemory(uint64_t address, uint64_t bytes, uint32_t key)
{
    NS_ABORT_MSG_IF(!key || !bytes || bytes > UINT64_MAX - address || m_regions.count(key),
                    "RDMA registration needs a new nonzero key and a non-wrapping region");
    m_regions.emplace(key, Region{address, bytes});
}

void
RdmaOperationEngine::UnregisterMemory(uint32_t key)
{
    NS_ABORT_MSG_IF(HasPending() || m_receiveReserved || !m_waits.empty(),
                    "Drain RDMA operations, receives and waits before unregistering memory");
    m_regions.erase(key);
}

void
RdmaOperationEngine::WriteMemory(uint64_t address, const std::vector<uint8_t>& bytes)
{
    NS_ABORT_MSG_IF(!InAnyRegion(address, bytes.size()),
                    "Local RDMA memory access outside registration");
    auto begin = m_memory.lower_bound(address), end = m_memory.lower_bound(address + bytes.size());
    m_memory.erase(begin, end);
    for (size_t i = 0; i < bytes.size(); ++i)
    {
        if (bytes[i])
        {
            m_memory[address + i] = bytes[i];
        }
    }
    CheckWaits();
}

std::vector<uint8_t>
RdmaOperationEngine::ReadMemory(uint64_t address, uint32_t bytes) const
{
    NS_ABORT_MSG_IF(!InAnyRegion(address, bytes), "Local RDMA memory access outside registration");
    std::vector<uint8_t> result(bytes);
    for (auto it = m_memory.lower_bound(address);
         it != m_memory.end() && it->first - address < bytes;
         ++it)
    {
        result[it->first - address] = it->second;
    }
    return result;
}

void
RdmaOperationEngine::WriteWord(uint64_t address, uint64_t value)
{
    NS_ABORT_MSG_IF(address % 8, "RDMA atomic/signal address must be 8-byte aligned");
    WriteMemory(address, WordBytes(value));
}

uint64_t
RdmaOperationEngine::ReadWord(uint64_t address) const
{
    NS_ABORT_MSG_IF(address % 8, "RDMA atomic/signal address must be 8-byte aligned");
    auto bytes = ReadMemory(address, 8);
    uint64_t value = 0;
    for (uint32_t i = 0; i < 8; ++i)
    {
        value |= uint64_t(bytes[i]) << (8 * i);
    }
    return value;
}

Ptr<Packet>
RdmaOperationEngine::Payload(uint64_t address, uint32_t bytes, bool& zero) const
{
    auto first = m_memory.lower_bound(address);
    zero = first == m_memory.end() || first->first - address >= bytes;
    if (zero)
    {
        return Create<Packet>(bytes);
    }
    auto data = ReadMemory(address, bytes);
    return Create<Packet>(data.data(), bytes);
}

void
RdmaOperationEngine::Store(uint64_t address, Ptr<Packet> packet, uint32_t bytes, bool zero)
{
    if (zero)
    {
        m_memory.erase(m_memory.lower_bound(address), m_memory.lower_bound(address + bytes));
        CheckWaits();
    }
    else
    {
        std::vector<uint8_t> data(bytes);
        packet->CopyData(data.data(), bytes);
        WriteMemory(address, data);
    }
}

bool
RdmaOperationEngine::TestSignal(uint64_t address, RdmaComparison comparison, uint64_t value) const
{
    return Compare(ReadWord(address), comparison, value);
}

uint64_t
RdmaOperationEngine::WaitSignal(uint64_t address,
                                RdmaComparison comparison,
                                uint64_t value,
                                std::function<void(uint64_t)> complete)
{
    NS_ABORT_MSG_IF(!complete, "RDMA wait needs a callback");
    ReadWord(address);
    uint64_t id = m_nextWait++;
    m_waits.push_back({id, address, value, comparison, std::move(complete)});
    CheckWaits();
    return id;
}

void
RdmaOperationEngine::CancelSignalWait(uint64_t id)
{
    std::erase_if(m_waits, [id](const Wait& wait) { return wait.id == id; });
}

void
RdmaOperationEngine::CheckWaits()
{
    std::vector<std::pair<std::function<void(uint64_t)>, uint64_t>> ready;
    for (auto it = m_waits.begin(); it != m_waits.end();)
    {
        uint64_t actual = ReadWord(it->address);
        if (Compare(actual, it->comparison, it->value))
        {
            ready.emplace_back(std::move(it->complete), actual);
            it = m_waits.erase(it);
        }
        else
        {
            ++it;
        }
    }
    for (auto& [callback, value] : ready)
    {
        Schedule(m_model->GetConfig().signalPoll, [callback, value] { callback(value); });
    }
}

void
RdmaOperationEngine::PostReceive(uint64_t id,
                                 uint64_t address,
                                 uint64_t bytes,
                                 ReceiveCompletion complete,
                                 uint16_t peer,
                                 uint32_t qp,
                                 uint32_t key)
{
    NS_ABORT_MSG_IF(!m_model || !complete || !InRegion(key, address, bytes),
                    "Invalid RDMA receive buffer");
    NS_ABORT_MSG_IF(m_receiveReserved >= m_model->GetConfig().receiveQueueDepth,
                    "RDMA receive queue is full");
    NS_ABORT_MSG_IF(qp != UINT32_MAX && qp >= m_model->GetConfig().qpsPerPeer,
                    "Receive QP out of range");
    ++m_receiveReserved;
    ++m_stats.postedReceives;
    auto receive = std::make_shared<PostedReceive>(
        PostedReceive{id, address, bytes, key, qp, peer, std::move(complete)});
    m_receivePostFree =
        std::max(Simulator::Now(), m_receivePostFree) + m_model->GetConfig().receivePost;
    Schedule(m_receivePostFree - Simulator::Now(),
             [this, receive] { m_receives.push_back(receive); });
}

std::shared_ptr<RdmaOperationEngine::PostedReceive>
RdmaOperationEngine::MatchReceive(uint16_t peer, uint32_t qp)
{
    for (auto it = m_receives.begin(); it != m_receives.end(); ++it)
    {
        if (((*it)->peer == UINT16_MAX || (*it)->peer == peer) &&
            ((*it)->qp == UINT32_MAX || (*it)->qp == qp))
        {
            auto result = *it;
            m_receives.erase(it);
            --m_receiveReserved;
            return result;
        }
    }
    return {};
}

uint64_t
RdmaOperationEngine::Submit(const RdmaRequest& input, Completion complete)
{
    NS_ABORT_MSG_IF(!m_model || m_disposed || input.peer == m_rank,
                    "RDMA needs configured remote endpoints");
    RdmaRequest request = input;
    if (RdmaUsesReadAtomicCredit(request.opcode) && request.opcode != RdmaOpcode::READ)
    {
        request.bytes = 8;
    }
    NS_ABORT_MSG_IF(!request.bytes ||
                        (request.opcode != RdmaOpcode::SIGNAL &&
                         !InRegion(request.localKey, request.localAddress, request.bytes)),
                    "RDMA local buffer is unregistered or too small");
    if (request.opcode == RdmaOpcode::FETCH_ADD || request.opcode == RdmaOpcode::COMPARE_SWAP ||
        request.opcode == RdmaOpcode::SIGNAL || request.opcode == RdmaOpcode::PUT_SIGNAL)
    {
        NS_ABORT_MSG_IF(
            (request.opcode == RdmaOpcode::SIGNAL || request.opcode == RdmaOpcode::PUT_SIGNAL
                 ? request.signalAddress
                 : request.remoteAddress) %
                8,
            "RDMA atomic/signal address must be aligned");
    }
    NS_ABORT_MSG_IF(
        (request.opcode == RdmaOpcode::FETCH_ADD || request.opcode == RdmaOpcode::COMPARE_SWAP) &&
            request.localAddress % 8,
        "RDMA atomic result address must be aligned");
    if (request.opcode == RdmaOpcode::PUT_SIGNAL)
    {
        NS_ABORT_MSG_IF(request.bytes > UINT64_MAX - request.remoteAddress ||
                            request.signalAddress > UINT64_MAX - 8,
                        "RDMA put/signal address range wraps");
        NS_ABORT_MSG_IF(request.signalAddress < request.remoteAddress + request.bytes &&
                            request.remoteAddress < request.signalAddress + 8,
                        "RDMA put payload and signal must not overlap");
    }
    auto result = std::make_shared<uint64_t>(0), identity = std::make_shared<uint64_t>(0);
    uint64_t id = m_model->SubmitOperation(
        request.opcode,
        request.peer,
        request.bytes,
        [this, request, result](uint64_t child) { Execute(request, child, result); },
        request.producer,
        [this, request, result, identity, complete](bool success) {
            std::erase_if(m_sources, [identity](const auto& item) {
                return item.second->tag.requestId == *identity;
            });
            if (complete)
            {
                complete(
                    RdmaCompletion{*identity, request.opcode, success, request.bytes, *result});
            }
        });
    *identity = id;
    return id;
}

void
RdmaOperationEngine::Execute(const RdmaRequest& request,
                             uint64_t id,
                             std::shared_ptr<uint64_t> result)
{
    auto timing = m_model->GetWorkTiming(id);
    if (timing.opcode == RdmaOpcode::SIGNAL && !m_model->RequestSucceeded(id))
    {
        m_model->NotifyOperationComplete(id, false);
        return;
    }
    auto source = std::make_shared<Source>();
    source->result = result;
    source->qp = timing.qp;
    source->flow = request.flowId;
    source->vc = request.vc;
    auto& tag = source->tag;
    tag.opcode = timing.opcode;
    tag.owner = m_rank;
    tag.workId = id;
    tag.requestId = timing.requestId;
    tag.bytes = timing.bytes;
    tag.offset = timing.offset;
    tag.localAddress = request.localAddress;
    tag.remoteAddress = request.remoteAddress;
    tag.localKey = request.localKey;
    tag.remoteKey = request.remoteKey;
    tag.operand = request.operand;
    tag.compare = request.compare;
    tag.immediate = request.immediate;
    tag.signalAddress = request.signalAddress;
    tag.signalValue = request.signalValue;
    tag.signalOp = request.signalOp;
    if (tag.opcode == RdmaOpcode::SIGNAL)
    {
        tag.offset = 0;
    }
    m_sources[id] = source;
    SendSource(source);
}

void
RdmaOperationEngine::SendSource(std::shared_ptr<Source> source)
{
    auto tag = source->tag;
    if (RdmaHasSourcePayload(tag.opcode))
    {
        SendPayload(m_model->GetWorkTiming(tag.workId).peer,
                    tag,
                    source->qp,
                    source->flow,
                    source->vc,
                    tag.localAddress + tag.offset,
                    tag.bytes);
        return;
    }
    tag.phase = RdmaPhase::REQUEST;
    uint32_t bytes =
        tag.opcode == RdmaOpcode::READ ? 16 : (tag.opcode == RdmaOpcode::SIGNAL ? 20 : 28);
    m_stats.wireControlBytes += bytes;
    m_transmit(m_model->GetWorkTiming(tag.workId).peer,
               Create<Packet>(bytes),
               tag,
               source->qp,
               source->flow,
               source->vc,
               0);
}

void
RdmaOperationEngine::SendPayload(uint16_t peer,
                                 RdmaOperationTag tag,
                                 uint32_t qp,
                                 uint16_t flow,
                                 uint8_t vc,
                                 uint64_t address,
                                 uint64_t bytes)
{
    // The endpoint frames every operation body with the existing RoCE model.
    uint32_t cap = m_payloadLimit ? m_payloadLimit(peer)
                                  : std::max(1u,
                                             m_model->GetConfig().mtuBytes > 14
                                                 ? m_model->GetConfig().mtuBytes - 14
                                                 : 1u);
    // Round 62: a payload-limit callback returning 0 with a nonzero
    // request stalls forever here — offset never advances while the loop
    // keeps emitting zero-length pieces (codex probe: SetPayloadLimit(0)
    // then SendPayload(..., bytes=1)). The default fallback is floored
    // at 1, so only an explicit callback can trip this.
    NS_ABORT_MSG_IF(cap == 0 && bytes > 0,
                    "RdmaOperationEngine::SendPayload zero payload-limit cap for a "
                        << bytes << "-B operation");
    for (uint64_t offset = 0; offset < bytes;)
    {
        uint32_t count = static_cast<uint32_t>(std::min<uint64_t>(cap, bytes - offset));
        auto piece = tag;
        piece.fragmentOffset = offset;
        auto packet = Payload(address + offset, count, piece.zeroPayload);
        uint32_t immediate = (offset == 0 && (tag.opcode == RdmaOpcode::SEND_IMM ||
                                              tag.opcode == RdmaOpcode::WRITE_IMM))
                                 ? 4
                                 : 0;
        m_stats.wireDataBytes += count;
        m_stats.wireControlBytes += immediate;
        Time delay = m_model->ReserveSourceRead(count);
        Schedule(delay, [this, peer, packet, piece, qp, flow, vc, immediate] {
            m_transmit(peer, packet, piece, qp, flow, vc, immediate);
        });
        offset += count;
    }
}

void
RdmaOperationEngine::Acknowledge(const FabricHeader& header, const RdmaOperationTag* completion)
{
    if (completion)
    {
        m_ackMetadata[AckKey(header)] = *completion;
    }
    m_hold(header, false);
    m_ack(header);
}

void
RdmaOperationEngine::DecorateAck(const FabricHeader& received, Ptr<Packet> packet) const
{
    auto found = m_ackMetadata.find(AckKey(received));
    if (found != m_ackMetadata.end())
    {
        packet->AddPacketTag(found->second);
    }
}

void
RdmaOperationEngine::PacketSent(const FabricHeader& header, Ptr<const Packet> packet)
{
    RdmaOperationTag tag;
    if (!packet->PeekPacketTag(tag) || header.GetPacketType() != FabricPacketType::DATA)
    {
        return;
    }
    m_sent[TxKey(header)] = {tag, header.GetEffectiveDataSize()};
}

void
RdmaOperationEngine::FailSource(uint64_t id)
{
    auto found = m_sources.find(id);
    if (found != m_sources.end() && !found->second->done)
    {
        found->second->done = true;
        m_model->NotifyOperationComplete(id, false);
    }
}

void
RdmaOperationEngine::PacketAcknowledged(const FabricHeader& header, Ptr<const Packet> packet)
{
    auto sent = m_sent.find(AckKey(header));
    if (sent != m_sent.end())
    {
        auto saved = sent->second;
        m_sent.erase(sent);
        if (saved.tag.phase == RdmaPhase::READ_RESPONSE ||
            saved.tag.phase == RdmaPhase::ATOMIC_RESPONSE)
        {
            Key key{saved.tag.owner, saved.tag.workId};
            auto responder = m_responders.find(key);
            if (responder != m_responders.end())
            {
                responder->second->acknowledged += saved.bytes;
                if (responder->second->acknowledged >= saved.tag.bytes)
                {
                    FinishResponder(key);
                }
            }
        }
    }
    RdmaOperationTag tag;
    if (!packet->PeekPacketTag(tag) || tag.owner != m_rank)
    {
        return;
    }
    auto source = m_sources.find(tag.workId);
    if (source == m_sources.end() || source->second->done)
    {
        return;
    }
    auto state = source->second;
    if (tag.phase == RdmaPhase::COMPLETE)
    {
        state->done = true;
        m_model->NotifyOperationComplete(tag.workId, tag.success);
    }
    else if (tag.phase == RdmaPhase::RNR && tag.attempt == state->tag.attempt &&
             !state->retryPending)
    {
        if (state->tag.attempt >= m_model->GetConfig().rnrRetryLimit)
        {
            ++m_stats.rnrExhausted;
            FailSource(tag.workId);
            return;
        }
        state->retryPending = true;
        ++m_stats.rnrRetries;
        Schedule(m_model->GetConfig().rnrRetry, [this, state] {
            if (state->done)
            {
                return;
            }
            ++state->tag.attempt;
            state->retryPending = false;
            SendSource(state);
        });
    }
}

void
RdmaOperationEngine::PermanentLoss(const FabricHeader& header)
{
    auto found = m_sent.find(TxKey(header));
    if (found != m_sent.end() && found->second.tag.owner == m_rank)
    {
        m_model->NotifyFailure(found->second.tag.workId);
    }
}

bool
RdmaOperationEngine::Receive(Ptr<Packet> packet, const FabricHeader& header)
{
    RdmaOperationTag tag;
    if (!packet->PeekPacketTag(tag))
    {
        return false;
    }
    RdmaPacketTag work;
    NS_ABORT_MSG_IF(!packet->PeekPacketTag(work), "Missing RDMA work metadata");
    if (tag.phase == RdmaPhase::REQUEST)
    {
        uint64_t address = tag.opcode == RdmaOpcode::SIGNAL ? tag.signalAddress : tag.remoteAddress;
        bool valid = tag.offset <= UINT64_MAX - address &&
                     InRegion(tag.remoteKey, address + tag.offset, tag.bytes);
        if (!valid)
        {
            tag.phase = RdmaPhase::COMPLETE;
            tag.success = false;
            Acknowledge(header, &tag);
            return true;
        }
        if (tag.opcode != RdmaOpcode::READ && address % 8)
        {
            tag.phase = RdmaPhase::COMPLETE;
            tag.success = false;
            Acknowledge(header, &tag);
            return true;
        }
        if (tag.opcode != RdmaOpcode::SIGNAL)
        {
            Acknowledge(header);
        }
        else
        {
            m_hold(header, true);
        }
        QueueResponder(tag, work.qp, header);
        return true;
    }
    AcceptData(packet, header, tag, work.qp);
    return true;
}

void
RdmaOperationEngine::AcceptData(Ptr<Packet> packet,
                                const FabricHeader& header,
                                RdmaOperationTag tag,
                                uint32_t qp)
{
    bool response =
        tag.phase == RdmaPhase::READ_RESPONSE || tag.phase == RdmaPhase::ATOMIC_RESPONSE;
    if (response && !m_sources.count(tag.workId))
    {
        Acknowledge(header);
        return;
    }
    Key key{tag.owner, tag.workId};
    auto found = m_incoming.find(key);
    if (m_failedIncoming.count(key))
    {
        tag.phase = RdmaPhase::COMPLETE;
        tag.success = false;
        Acknowledge(header, &tag);
        return;
    }
    auto rejected = m_rejectedAttempts.find(key);
    if (!response && rejected != m_rejectedAttempts.end() && tag.attempt <= rejected->second)
    {
        tag.phase = RdmaPhase::RNR;
        Acknowledge(header, &tag);
        return;
    }
    if (found != m_incoming.end() && tag.attempt < found->second->tag.attempt)
    {
        Acknowledge(header);
        return;
    }
    if (found == m_incoming.end())
    {
        auto incoming = std::make_shared<Incoming>();
        incoming->tag = tag;
        incoming->qp = qp;
        incoming->response = response;
        if (!response && RdmaNeedsReceive(tag.opcode))
        {
            incoming->receive = MatchReceive(header.GetSourceRank(), qp);
            if (!incoming->receive)
            {
                m_rejectedAttempts[key] = tag.attempt;
                tag.phase = RdmaPhase::RNR;
                Acknowledge(header, &tag);
                return;
            }
        }
        if (!response && (tag.opcode == RdmaOpcode::SEND || tag.opcode == RdmaOpcode::SEND_IMM))
        {
            incoming->address = incoming->receive->address;
        }
        else
        {
            uint64_t base = response ? tag.localAddress : tag.remoteAddress;
            if (tag.offset > UINT64_MAX - base)
            {
                tag.phase = RdmaPhase::COMPLETE;
                tag.success = false;
                Acknowledge(header, &tag);
                return;
            }
            incoming->address = base + tag.offset;
        }
        uint32_t keyToCheck = response ? tag.localKey : tag.remoteKey;
        bool valid = (!incoming->receive || tag.opcode == RdmaOpcode::WRITE_IMM ||
                      tag.bytes <= incoming->receive->bytes);
        if (!response && (tag.opcode == RdmaOpcode::SEND || tag.opcode == RdmaOpcode::SEND_IMM))
        {
            keyToCheck = incoming->receive->key;
        }
        valid = valid && InRegion(keyToCheck, incoming->address, tag.bytes);
        if (!valid)
        {
            m_failedIncoming.insert(key);
            tag.phase = RdmaPhase::COMPLETE;
            tag.success = false;
            Acknowledge(header, &tag);
            if (incoming->receive)
            {
                auto cb = incoming->receive->complete;
                RdmaReceiveCompletion result;
                result.receiveId = incoming->receive->id;
                result.peer = header.GetSourceRank();
                result.opcode = tag.opcode;
                result.success = false;
                Schedule(Time(0), [this, cb, result] {
                    ++m_stats.receiveCompletions;
                    cb(result);
                });
            }
            return;
        }
        m_incoming[key] = incoming;
        found = m_incoming.find(key);
        if (m_model->GetConfig().dataPath == "staged")
        {
            m_model->AcquireReceiveStaging(tag.bytes, [this, key, incoming] {
                incoming->allocated = true;
                auto waiting = std::move(incoming->waiting);
                for (auto& piece : waiting)
                {
                    StartPiece(key, incoming, std::move(piece));
                }
            });
        }
        else
        {
            incoming->allocated = true;
        }
    }
    m_hold(header, true);
    Piece piece{packet, header, tag};
    auto incoming = found->second;
    if (incoming->allocated)
    {
        StartPiece(key, incoming, std::move(piece));
    }
    else
    {
        incoming->waiting.push_back(std::move(piece));
    }
}

void
RdmaOperationEngine::StartPiece(Key key, std::shared_ptr<Incoming> incoming, Piece piece)
{
    uint32_t bytes = piece.header.GetEffectiveDataSize();
    NS_ABORT_MSG_IF(piece.tag.fragmentOffset > incoming->tag.bytes ||
                        bytes > incoming->tag.bytes - piece.tag.fragmentOffset,
                    "RDMA payload exceeds its work request");
    Schedule(m_model->ReserveDestinationWrite(bytes), [this, key, incoming, piece, bytes] {
        if (m_model->GetConfig().dataPath == "staged")
        {
            incoming->staged.push_back(piece);
        }
        else
        {
            Store(incoming->address + piece.tag.fragmentOffset,
                  piece.packet,
                  bytes,
                  piece.tag.zeroPayload);
        }
        incoming->received += bytes;
        NS_ABORT_MSG_IF(incoming->received > incoming->tag.bytes,
                        "RDMA receive byte ledger overflow");
        if (incoming->received < incoming->tag.bytes)
        {
            Acknowledge(piece.header);
            return;
        }
        if (m_model->GetConfig().dataPath == "staged")
        {
            Schedule(m_model->ReserveDestinationH2d(incoming->tag.bytes),
                     [this, key, incoming, piece] {
                         for (auto& saved : incoming->staged)
                         {
                             Store(incoming->address + saved.tag.fragmentOffset,
                                   saved.packet,
                                   saved.header.GetEffectiveDataSize(),
                                   saved.tag.zeroPayload);
                         }
                         m_model->ReleaseReceiveStaging(incoming->tag.bytes);
                         CompleteIncoming(key, incoming, piece);
                     });
        }
        else
        {
            CompleteIncoming(key, incoming, piece);
        }
    });
}

void
RdmaOperationEngine::CompleteIncoming(Key key,
                                      std::shared_ptr<Incoming> incoming,
                                      const Piece& last)
{
    m_model->NotifyDataReady();
    m_incoming.erase(key);
    auto tag = incoming->tag;
    if (incoming->response)
    {
        auto source = m_sources.find(tag.workId);
        if (source != m_sources.end() && !source->second->done)
        {
            if (tag.phase == RdmaPhase::ATOMIC_RESPONSE)
            {
                *source->second->result = ReadWord(incoming->address);
            }
            else
            {
                m_stats.readResponseBytes += tag.bytes;
            }
            source->second->done = true;
            m_model->NotifyOperationComplete(tag.workId, tag.success);
        }
        Acknowledge(last.header);
    }
    else
    {
        tag.phase = RdmaPhase::COMPLETE;
        Acknowledge(last.header, &tag);
        if (incoming->receive)
        {
            RdmaReceiveCompletion result;
            result.receiveId = incoming->receive->id;
            result.requestId = tag.requestId;
            result.offset = tag.offset;
            result.peer = tag.owner;
            result.qp = incoming->qp;
            result.opcode = tag.opcode;
            result.bytes = tag.bytes;
            result.hasImmediate =
                tag.opcode == RdmaOpcode::SEND_IMM || tag.opcode == RdmaOpcode::WRITE_IMM;
            result.immediate = tag.immediate;
            auto cb = incoming->receive->complete;
            const auto& config = m_model->GetConfig();
            Schedule(config.cqWrite + (config.initiator == "cpu" ? config.cpuPoll : config.gpuPoll),
                     [this, cb, result]() mutable {
                         ++m_stats.receiveCompletions;
                         result.observed = Simulator::Now();
                         cb(result);
                     });
        }
    }
}

void
RdmaOperationEngine::QueueResponder(RdmaOperationTag tag, uint32_t qp, const FabricHeader& header)
{
    auto responder = std::make_shared<Responder>();
    responder->tag = tag;
    responder->qp = qp;
    responder->flow = header.GetFlowId();
    responder->vc = header.GetVirtualChannel();
    responder->header = header;
    m_responderWaiting.push_back(responder);
    PumpResponders();
}

void
RdmaOperationEngine::PumpResponders()
{
    while (!m_responderWaiting.empty() &&
           m_stats.activeResponses < m_model->GetConfig().responderDepth)
    {
        auto responder = m_responderWaiting.front();
        m_responderWaiting.pop_front();
        m_responders[{responder->tag.owner, responder->tag.workId}] = responder;
        ++m_stats.activeResponses;
        m_stats.responsePeak = std::max(m_stats.responsePeak, m_stats.activeResponses);
        StartResponder(responder);
    }
}

void
RdmaOperationEngine::StartResponder(std::shared_ptr<Responder> responder)
{
    auto tag = responder->tag;
    if (tag.opcode == RdmaOpcode::READ)
    {
        ++m_stats.readRequests;
        auto send = [this, responder] {
            auto reply = responder->tag;
            reply.phase = RdmaPhase::READ_RESPONSE;
            Schedule(m_model->GetConfig().readService, [this, responder, reply] {
                SendPayload(reply.owner,
                            reply,
                            responder->qp,
                            responder->flow,
                            responder->vc,
                            reply.remoteAddress + reply.offset,
                            reply.bytes);
            });
        };
        if (m_model->GetConfig().dataPath == "staged")
        {
            responder->staged = true;
            m_model->StageSourcePayload(tag.bytes, std::move(send));
        }
        else
        {
            send();
        }
    }
    else
    {
        uint64_t address = tag.opcode == RdmaOpcode::SIGNAL ? tag.signalAddress : tag.remoteAddress;
        auto& queue = m_atomics[address];
        queue.push_back(responder);
        if (queue.size() == 1)
        {
            RunAtomic(responder);
        }
    }
}

void
RdmaOperationEngine::RunAtomic(std::shared_ptr<Responder> responder)
{
    auto run = [this, responder] {
        Time service = m_model->ReserveSourceRead(8) + m_model->ReserveDestinationWrite(8) +
                       m_model->GetConfig().atomicService;
        Schedule(service, [this, responder] {
            auto tag = responder->tag;
            uint64_t address =
                tag.opcode == RdmaOpcode::SIGNAL ? tag.signalAddress : tag.remoteAddress;
            uint64_t old = ReadWord(address), next = old;
            if (tag.opcode == RdmaOpcode::FETCH_ADD)
            {
                next = old + tag.operand;
            }
            else if (tag.opcode == RdmaOpcode::COMPARE_SWAP)
            {
                if (old == tag.compare)
                {
                    next = tag.operand;
                }
            }
            else
            {
                next = tag.signalOp == RdmaSignalOp::SET ? tag.signalValue : old + tag.signalValue;
            }
            auto commit = [this, responder, address, old, next] {
                auto reply = responder->tag;
                WriteWord(address, next);
                m_model->NotifyDataReady();
                if (reply.opcode == RdmaOpcode::SIGNAL)
                {
                    ++m_stats.signalUpdates;
                    reply.phase = RdmaPhase::COMPLETE;
                    Acknowledge(responder->header, &reply);
                    FinishResponder({reply.owner, reply.workId});
                }
                else
                {
                    ++m_stats.atomicExecutions;
                    reply.phase = RdmaPhase::ATOMIC_RESPONSE;
                    reply.bytes = 8;
                    reply.offset = 0;
                    auto data = WordBytes(old);
                    reply.zeroPayload = old == 0;
                    m_stats.wireDataBytes += 8;
                    auto packet = Create<Packet>(data.data(), 8);
                    Schedule(m_model->ReserveSourceRead(8), [this, responder, reply, packet] {
                        m_transmit(reply.owner,
                                   packet,
                                   reply,
                                   responder->qp,
                                   responder->flow,
                                   responder->vc,
                                   0);
                    });
                }
                auto found = m_atomics.find(address);
                found->second.pop_front();
                if (found->second.empty())
                {
                    m_atomics.erase(found);
                }
                else
                {
                    RunAtomic(found->second.front());
                }
            };
            if (m_model->GetConfig().dataPath == "staged")
            {
                m_model->AcquireReceiveStaging(8, [this, commit] {
                    Schedule(m_model->ReserveDestinationH2d(8), [this, commit] {
                        m_model->ReleaseReceiveStaging(8);
                        commit();
                    });
                });
            }
            else
            {
                commit();
            }
        });
    };
    if (m_model->GetConfig().dataPath == "staged")
    {
        responder->staged = true;
        m_model->StageSourcePayload(8, std::move(run));
    }
    else
    {
        run();
    }
}

void
RdmaOperationEngine::FinishResponder(Key key)
{
    auto found = m_responders.find(key);
    if (found == m_responders.end())
    {
        return;
    }
    auto responder = found->second;
    if (responder->staged)
    {
        m_model->ReleaseSourceStaging(responder->tag.bytes);
    }
    m_responders.erase(found);
    --m_stats.activeResponses;
    PumpResponders();
}

void
RdmaOperationEngine::DoDispose()
{
    m_disposed = true;
    for (auto& event : m_events)
    {
        if (event.IsPending())
        {
            Simulator::Cancel(event);
        }
    }
    m_events.clear();
    m_sources.clear();
    m_incoming.clear();
    m_rejectedAttempts.clear();
    m_failedIncoming.clear();
    m_responders.clear();
    m_responderWaiting.clear();
    m_atomics.clear();
    m_receives.clear();
    m_waits.clear();
    m_sent.clear();
    m_ackMetadata.clear();
    m_transmit = {};
    m_ack = {};
    m_hold = {};
    m_model = nullptr;
    Object::DoDispose();
}
} // namespace ns3
