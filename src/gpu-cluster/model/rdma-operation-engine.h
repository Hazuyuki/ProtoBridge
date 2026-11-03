/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef RDMA_OPERATION_ENGINE_H
#define RDMA_OPERATION_ENGINE_H

#include "fabric-header.h"
#include "rdma-submission-model.h"

#include "ns3/packet.h"

#include <deque>
#include <map>
#include <set>
#include <tuple>
#include <vector>

namespace ns3
{
/** Stateful RC operations above the shared PEX packet/retry path. */
class RdmaOperationEngine : public Object
{
  public:
    static TypeId GetTypeId();
    using Completion = std::function<void(const RdmaCompletion&)>;
    using ReceiveCompletion = std::function<void(const RdmaReceiveCompletion&)>;
    using Transmit = std::function<void(uint16_t,
                                        Ptr<Packet>,
                                        const RdmaOperationTag&,
                                        uint32_t,
                                        uint16_t,
                                        uint8_t,
                                        uint32_t)>;
    using Ack = std::function<void(const FabricHeader&)>;
    using HoldAck = std::function<void(const FabricHeader&, bool)>;
    void Configure(uint16_t rank,
                   Ptr<RdmaSubmissionModel> model,
                   Transmit transmit,
                   Ack ack,
                   HoldAck hold);
    void SetPayloadLimit(std::function<uint32_t(uint16_t)> limit);
    uint64_t Submit(const RdmaRequest& request, Completion complete = {});
    void RegisterMemory(uint64_t address, uint64_t bytes, uint32_t key = 1);
    void UnregisterMemory(uint32_t key);
    void WriteMemory(uint64_t address, const std::vector<uint8_t>& data);
    std::vector<uint8_t> ReadMemory(uint64_t address, uint32_t bytes) const;
    void WriteWord(uint64_t address, uint64_t value);
    uint64_t ReadWord(uint64_t address) const;
    void PostReceive(uint64_t receiveId,
                     uint64_t address,
                     uint64_t bytes,
                     ReceiveCompletion complete,
                     uint16_t peer = UINT16_MAX,
                     uint32_t qp = UINT32_MAX,
                     uint32_t key = 1);
    bool TestSignal(uint64_t address, RdmaComparison comparison, uint64_t value) const;
    uint64_t WaitSignal(uint64_t address,
                        RdmaComparison comparison,
                        uint64_t value,
                        std::function<void(uint64_t)> complete);
    void CancelSignalWait(uint64_t id);
    bool Receive(Ptr<Packet> packet, const FabricHeader& header);
    void PacketSent(const FabricHeader& header, Ptr<const Packet> packet);
    void PacketAcknowledged(const FabricHeader& header, Ptr<const Packet> packet);
    void DecorateAck(const FabricHeader& received, Ptr<Packet> ack) const;
    void PermanentLoss(const FabricHeader& header);
    const RdmaOperationStats& GetStats() const;
    bool HasPending() const;

  protected:
    void DoDispose() override;

  private:
    using Key = std::pair<uint16_t, uint64_t>;
    using PacketKey = std::tuple<uint16_t, uint8_t, uint16_t, uint32_t>;

    struct Region
    {
        uint64_t base, bytes;
    };

    struct PostedReceive
    {
        uint64_t id, address, bytes;
        uint32_t key, qp;
        uint16_t peer;
        ReceiveCompletion complete;
    };

    struct Source
    {
        RdmaOperationTag tag;
        uint32_t qp;
        uint16_t flow;
        uint8_t vc;
        bool retryPending{false}, done{false};
        std::shared_ptr<uint64_t> result;
    };

    struct Piece
    {
        Ptr<Packet> packet;
        FabricHeader header;
        RdmaOperationTag tag;
    };

    struct Incoming
    {
        RdmaOperationTag tag;
        uint32_t qp;
        uint64_t received{0}, address{0};
        bool allocated{false}, response{false};
        std::shared_ptr<PostedReceive> receive;
        std::vector<Piece> waiting, staged;
    };

    struct Responder
    {
        RdmaOperationTag tag;
        uint32_t qp;
        uint16_t flow;
        uint8_t vc;
        uint64_t acknowledged{0};
        bool staged{false};
        FabricHeader header;
    };

    struct Wait
    {
        uint64_t id, address, value;
        RdmaComparison comparison;
        std::function<void(uint64_t)> complete;
    };

    void Schedule(Time delay, std::function<void()> action);
    std::function<void()> Guard(std::function<void()> action);
    bool InRegion(uint32_t key, uint64_t address, uint64_t bytes) const;
    bool InAnyRegion(uint64_t address, uint64_t bytes) const;
    Ptr<Packet> Payload(uint64_t address, uint32_t bytes, bool& zero) const;
    void Store(uint64_t address, Ptr<Packet> packet, uint32_t bytes, bool zero);
    void CheckWaits();
    void Execute(const RdmaRequest& request, uint64_t id, std::shared_ptr<uint64_t> result);
    void SendSource(std::shared_ptr<Source> source);
    void SendPayload(uint16_t peer,
                     RdmaOperationTag tag,
                     uint32_t qp,
                     uint16_t flow,
                     uint8_t vc,
                     uint64_t address,
                     uint64_t bytes);
    void Acknowledge(const FabricHeader& header, const RdmaOperationTag* completion = nullptr);
    void CompleteIncoming(Key key, std::shared_ptr<Incoming> incoming, const Piece& last);
    void StartPiece(Key key, std::shared_ptr<Incoming> incoming, Piece piece);
    void AcceptData(Ptr<Packet> packet,
                    const FabricHeader& header,
                    RdmaOperationTag tag,
                    uint32_t qp);
    void QueueResponder(RdmaOperationTag tag, uint32_t qp, const FabricHeader& header);
    void PumpResponders();
    void StartResponder(std::shared_ptr<Responder> responder);
    void RunAtomic(std::shared_ptr<Responder> responder);
    void FinishResponder(Key key);
    std::shared_ptr<PostedReceive> MatchReceive(uint16_t peer, uint32_t qp);
    void FailSource(uint64_t id);
    uint16_t m_rank{0};
    bool m_disposed{false};
    std::shared_ptr<bool> m_alive{std::make_shared<bool>(true)};
    Ptr<RdmaSubmissionModel> m_model;
    Transmit m_transmit;
    Ack m_ack;
    HoldAck m_hold;
    std::function<uint32_t(uint16_t)> m_payloadLimit;
    RdmaOperationStats m_stats;
    std::map<uint32_t, Region> m_regions;
    std::map<uint64_t, uint8_t> m_memory;
    std::map<uint64_t, std::shared_ptr<Source>> m_sources;
    std::map<Key, std::shared_ptr<Incoming>> m_incoming;
    std::map<Key, uint32_t> m_rejectedAttempts;
    std::set<Key> m_failedIncoming;
    std::map<Key, std::shared_ptr<Responder>> m_responders;
    std::deque<std::shared_ptr<Responder>> m_responderWaiting;
    std::map<uint64_t, std::deque<std::shared_ptr<Responder>>> m_atomics;
    std::deque<std::shared_ptr<PostedReceive>> m_receives;
    uint64_t m_receiveReserved{0}, m_nextWait{1};
    Time m_receivePostFree;
    std::vector<Wait> m_waits;
    std::map<PacketKey, RdmaOperationTag> m_ackMetadata;

    struct Sent
    {
        RdmaOperationTag tag;
        uint64_t bytes;
    };

    std::map<PacketKey, Sent> m_sent;
    std::vector<EventId> m_events;
};
} // namespace ns3
#endif
