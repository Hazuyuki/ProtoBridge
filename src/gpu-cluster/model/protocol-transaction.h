/*
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef PROTOCOL_TRANSACTION_H
#define PROTOCOL_TRANSACTION_H

#include "fabric-header.h"

#include "ns3/event-id.h"
#include "ns3/nstime.h"
#include "ns3/object.h"

#include <cstdint>
#include <cstddef>
#include <functional>
#include <limits>
#include <queue>
#include <string>
#include <unordered_map>
#include <vector>

namespace ns3
{

using ProtocolTransactionNodeId = uint32_t;

constexpr ProtocolTransactionNodeId PROTOCOL_TRANSACTION_INVALID_NODE =
    std::numeric_limits<ProtocolTransactionNodeId>::max();
constexpr uint16_t PROTOCOL_TRANSACTION_ANY_RANK = std::numeric_limits<uint16_t>::max();
constexpr uint16_t PROTOCOL_TRANSACTION_ANY_FLOW = std::numeric_limits<uint16_t>::max();

enum class ProtocolTransactionKind : uint8_t
{
    DATA_TRANSFER,
    P2P_TRANSFER,
    MEMORY_READ,
    MEMORY_WRITE,
    COLLECTIVE_OFFLOAD
};

enum class ProtocolTransactionActionType : uint8_t
{
    SEND_DATA,
    SEND_P2P,
    SEND_MEMORY_READ,
    SEND_MEMORY_WRITE,
    SEND_COLLECTIVE,
    CUSTOM
};

enum class ProtocolTransactionEventType : uint8_t
{
    PACKET_DELIVERED,
    RESPONSE_RECEIVED,
    ACK_RECEIVED,
    CUSTOM
};

enum class ProtocolTransactionNodeType : uint8_t
{
    ACTION,
    WAIT,
    DELAY,
    COMPLETE
};

enum class ProtocolTransactionDependency : uint8_t
{
    ALL,
    ANY
};

enum class ProtocolTransactionNodeState : uint8_t
{
    PENDING,
    ACTIVE,
    COMPLETED
};

/**
 * Architecture-visible description of one logical protocol transfer.
 * Topology and route selection are intentionally absent.
 */
struct ProtocolTransactionRequest
{
    ProtocolTransactionKind kind{ProtocolTransactionKind::DATA_TRANSFER};
    FabricPacketType packetType{FabricPacketType::DATA};
    uint16_t sourceRank{0};
    uint16_t destinationRank{0};
    uint16_t flowId{0};
    uint8_t virtualChannel{0};
    uint64_t effectiveBytes{0};
    uint64_t responseBytes{0};
    uint64_t address{0};
    uint32_t stageId{0};
    bool hasProtocolId{false};
    uint8_t protocolId{0};
    std::string label;
};

/**
 * An action emitted by the transaction graph and bound to a simulator by the
 * executor's action callback.
 */
struct ProtocolTransactionAction
{
    ProtocolTransactionActionType type{ProtocolTransactionActionType::SEND_DATA};
    FabricPacketType packetType{FabricPacketType::DATA};
    uint16_t sourceRank{0};
    uint16_t destinationRank{0};
    uint16_t flowId{0};
    uint8_t virtualChannel{0};
    uint8_t protocolId{0};
    uint64_t effectiveBytes{0};
    uint64_t wireBytes{0};
    uint64_t chunkBytes{0};
    uint64_t address{0};
    uint32_t stageId{0};
};

struct ProtocolTransactionEvent
{
    ProtocolTransactionEventType type{ProtocolTransactionEventType::PACKET_DELIVERED};
    FabricPacketType packetType{FabricPacketType::DATA};
    uint16_t sourceRank{0};
    uint16_t destinationRank{0};
    uint16_t flowId{0};
    uint64_t bytes{0};
    uint32_t stageId{0};
};

struct ProtocolTransactionEventMatcher
{
    ProtocolTransactionEventType type{ProtocolTransactionEventType::PACKET_DELIVERED};
    uint16_t sourceRank{PROTOCOL_TRANSACTION_ANY_RANK};
    uint16_t destinationRank{PROTOCOL_TRANSACTION_ANY_RANK};
    uint16_t flowId{PROTOCOL_TRANSACTION_ANY_FLOW};
    uint32_t stageId{std::numeric_limits<uint32_t>::max()};
    bool matchPacketType{false};
    FabricPacketType packetType{FabricPacketType::DATA};

    bool Matches(const ProtocolTransactionEvent& event) const;
};

struct ProtocolTransactionNode
{
    ProtocolTransactionNodeId id{PROTOCOL_TRANSACTION_INVALID_NODE};
    ProtocolTransactionNodeType type{ProtocolTransactionNodeType::ACTION};
    ProtocolTransactionDependency dependency{ProtocolTransactionDependency::ALL};
    std::vector<ProtocolTransactionNodeId> dependencies;
    ProtocolTransactionAction action;
    ProtocolTransactionEventMatcher matcher;
    uint64_t targetBytes{0};
    uint32_t targetEvents{0};
    Time delay{Seconds(0)};
    std::string label;
};

/**
 * A finite event-dependency graph for one communication operation.
 */
class ProtocolTransactionGraph
{
  public:
    ProtocolTransactionNodeId AddAction(
        const ProtocolTransactionAction& action,
        const std::vector<ProtocolTransactionNodeId>& dependencies = {},
        const std::string& label = "");

    ProtocolTransactionNodeId AddWait(
        const ProtocolTransactionEventMatcher& matcher,
        uint64_t targetBytes,
        uint32_t targetEvents,
        const std::vector<ProtocolTransactionNodeId>& dependencies = {},
        const std::string& label = "");

    ProtocolTransactionNodeId AddDelay(
        Time delay,
        const std::vector<ProtocolTransactionNodeId>& dependencies = {},
        const std::string& label = "");

    ProtocolTransactionNodeId AddCompletion(
        const std::vector<ProtocolTransactionNodeId>& dependencies,
        ProtocolTransactionDependency dependency = ProtocolTransactionDependency::ALL,
        const std::string& label = "complete");

    void Clear();
    uint32_t GetNodeCount() const;
    const ProtocolTransactionNode& GetNode(ProtocolTransactionNodeId id) const;
    const std::vector<ProtocolTransactionNode>& GetNodes() const;
    bool Validate(std::string* error = nullptr) const;

  private:
    ProtocolTransactionNodeId AddNode(ProtocolTransactionNode node);

    std::vector<ProtocolTransactionNode> m_nodes;
};

/**
 * Executes a transaction graph. Packet actions leave through a callback;
 * packet-fabric outcomes return through NotifyEvent().
 */
class ProtocolTransactionExecutor : public Object
{
  public:
    static TypeId GetTypeId();

    using ActionCallback = std::function<void(const ProtocolTransactionAction&)>;
    using NodeCompletionCallback = std::function<void(ProtocolTransactionNodeId)>;
    using CompletionCallback = std::function<void()>;

    ProtocolTransactionExecutor();
    ~ProtocolTransactionExecutor() override;

    bool SetGraph(const ProtocolTransactionGraph& graph, std::string* error = nullptr);
    void SetActionCallback(ActionCallback callback);
    void SetNodeCompletionCallback(NodeCompletionCallback callback);
    void SetCompletionCallback(CompletionCallback callback);

    void Start();
    void NotifyEvent(const ProtocolTransactionEvent& event);
    void Reset();

    bool IsStarted() const;
    bool IsComplete() const;
    ProtocolTransactionNodeState GetNodeState(ProtocolTransactionNodeId id) const;
    uint64_t GetObservedBytes(ProtocolTransactionNodeId id) const;
    uint32_t GetObservedEvents(ProtocolTransactionNodeId id) const;

  protected:
    void DoDispose() override;

  private:
    struct RuntimeNode
    {
        ProtocolTransactionNodeState state{ProtocolTransactionNodeState::PENDING};
        uint64_t observedBytes{0};
        uint32_t observedEvents{0};
        uint32_t remainingDependencies{0};
        bool queued{false};
        EventId delayEvent;
    };

    struct WaitIndexKey
    {
        uint8_t eventType{0};
        uint16_t sourceRank{0};
        uint16_t destinationRank{0};
        uint16_t flowId{0};
        uint32_t stageId{0};
        uint8_t packetType{0};
        uint8_t wildcardMask{0};

        bool operator==(const WaitIndexKey& other) const;
    };

    struct WaitIndexKeyHash
    {
        std::size_t operator()(const WaitIndexKey& key) const;
    };

    void InitializeRuntime();
    static WaitIndexKey MakeWaitIndexKey(const ProtocolTransactionEventMatcher& matcher);
    void RegisterWait(const ProtocolTransactionNode& node);
    void PropagateCompletion(ProtocolTransactionNodeId id);
    void Advance();
    void OnDelayExpired(ProtocolTransactionNodeId id);
    void MarkCompleted(ProtocolTransactionNodeId id);
    void MaybeNotifyCompletion();

    ProtocolTransactionGraph m_graph;
    std::vector<RuntimeNode> m_runtime;
    std::vector<std::vector<ProtocolTransactionNodeId>> m_successors;
    std::queue<ProtocolTransactionNodeId> m_readyNodes;
    std::unordered_map<WaitIndexKey,
                       std::vector<ProtocolTransactionNodeId>,
                       WaitIndexKeyHash>
        m_waitIndex;
    ActionCallback m_actionCallback;
    NodeCompletionCallback m_nodeCompletionCallback;
    CompletionCallback m_completionCallback;
    bool m_graphSet{false};
    bool m_started{false};
    bool m_complete{false};
    bool m_completionNotified{false};
    bool m_advancing{false};
    bool m_advanceRequested{false};
};

} // namespace ns3

#endif // PROTOCOL_TRANSACTION_H
