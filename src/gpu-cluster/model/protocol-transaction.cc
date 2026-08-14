/*
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "protocol-transaction.h"

#include "ns3/assert.h"
#include "ns3/log.h"
#include "ns3/simulator.h"

#include <algorithm>
#include <functional>
#include <queue>
#include <utility>

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("ProtocolTransaction");
NS_OBJECT_ENSURE_REGISTERED(ProtocolTransactionExecutor);

bool
ProtocolTransactionEventMatcher::Matches(const ProtocolTransactionEvent& event) const
{
    if (type != event.type)
    {
        return false;
    }
    if (sourceRank != PROTOCOL_TRANSACTION_ANY_RANK && sourceRank != event.sourceRank)
    {
        return false;
    }
    if (destinationRank != PROTOCOL_TRANSACTION_ANY_RANK &&
        destinationRank != event.destinationRank)
    {
        return false;
    }
    if (flowId != PROTOCOL_TRANSACTION_ANY_FLOW && flowId != event.flowId)
    {
        return false;
    }
    if (stageId != std::numeric_limits<uint32_t>::max() && stageId != event.stageId)
    {
        return false;
    }
    return !matchPacketType || packetType == event.packetType;
}

ProtocolTransactionNodeId
ProtocolTransactionGraph::AddNode(ProtocolTransactionNode node)
{
    node.id = static_cast<ProtocolTransactionNodeId>(m_nodes.size());
    m_nodes.push_back(std::move(node));
    return m_nodes.back().id;
}

ProtocolTransactionNodeId
ProtocolTransactionGraph::AddAction(
    const ProtocolTransactionAction& action,
    const std::vector<ProtocolTransactionNodeId>& dependencies,
    const std::string& label)
{
    ProtocolTransactionNode node;
    node.type = ProtocolTransactionNodeType::ACTION;
    node.action = action;
    node.dependencies = dependencies;
    node.label = label;
    return AddNode(std::move(node));
}

ProtocolTransactionNodeId
ProtocolTransactionGraph::AddWait(
    const ProtocolTransactionEventMatcher& matcher,
    uint64_t targetBytes,
    uint32_t targetEvents,
    const std::vector<ProtocolTransactionNodeId>& dependencies,
    const std::string& label)
{
    ProtocolTransactionNode node;
    node.type = ProtocolTransactionNodeType::WAIT;
    node.matcher = matcher;
    node.targetBytes = targetBytes;
    node.targetEvents = targetEvents;
    node.dependencies = dependencies;
    node.label = label;
    return AddNode(std::move(node));
}

ProtocolTransactionNodeId
ProtocolTransactionGraph::AddDelay(
    Time delay,
    const std::vector<ProtocolTransactionNodeId>& dependencies,
    const std::string& label)
{
    ProtocolTransactionNode node;
    node.type = ProtocolTransactionNodeType::DELAY;
    node.delay = delay;
    node.dependencies = dependencies;
    node.label = label;
    return AddNode(std::move(node));
}

ProtocolTransactionNodeId
ProtocolTransactionGraph::AddCompletion(
    const std::vector<ProtocolTransactionNodeId>& dependencies,
    ProtocolTransactionDependency dependency,
    const std::string& label)
{
    ProtocolTransactionNode node;
    node.type = ProtocolTransactionNodeType::COMPLETE;
    node.dependencies = dependencies;
    node.dependency = dependency;
    node.label = label;
    return AddNode(std::move(node));
}

void
ProtocolTransactionGraph::Clear()
{
    m_nodes.clear();
}

uint32_t
ProtocolTransactionGraph::GetNodeCount() const
{
    return static_cast<uint32_t>(m_nodes.size());
}

const ProtocolTransactionNode&
ProtocolTransactionGraph::GetNode(ProtocolTransactionNodeId id) const
{
    NS_ASSERT_MSG(id < m_nodes.size(), "Protocol transaction node ID out of range");
    return m_nodes[id];
}

const std::vector<ProtocolTransactionNode>&
ProtocolTransactionGraph::GetNodes() const
{
    return m_nodes;
}

bool
ProtocolTransactionGraph::Validate(std::string* error) const
{
    auto fail = [error](const std::string& message) {
        if (error)
        {
            *error = message;
        }
        return false;
    };

    if (m_nodes.empty())
    {
        return fail("transaction graph is empty");
    }

    uint32_t completionCount = 0;
    ProtocolTransactionNodeId completionId = PROTOCOL_TRANSACTION_INVALID_NODE;
    std::vector<std::vector<ProtocolTransactionNodeId>> successors(m_nodes.size());
    std::vector<uint32_t> indegree(m_nodes.size(), 0);

    for (const auto& node : m_nodes)
    {
        if (node.id >= m_nodes.size())
        {
            return fail("transaction graph contains a non-contiguous node ID");
        }
        if (node.type == ProtocolTransactionNodeType::COMPLETE)
        {
            completionCount++;
            completionId = node.id;
        }
        if (node.type == ProtocolTransactionNodeType::WAIT &&
            node.targetBytes == 0 && node.targetEvents == 0)
        {
            return fail("wait node has no completion threshold");
        }
        if (node.type == ProtocolTransactionNodeType::DELAY && node.delay.IsStrictlyNegative())
        {
            return fail("delay node has a negative duration");
        }
        for (ProtocolTransactionNodeId dependency : node.dependencies)
        {
            if (dependency >= m_nodes.size())
            {
                return fail("transaction node depends on an unknown node");
            }
            if (dependency == node.id)
            {
                return fail("transaction node depends on itself");
            }
            successors[dependency].push_back(node.id);
            indegree[node.id]++;
        }
    }

    if (completionCount != 1)
    {
        return fail("transaction graph must contain exactly one completion node");
    }

    std::queue<ProtocolTransactionNodeId> ready;
    for (ProtocolTransactionNodeId id = 0; id < indegree.size(); ++id)
    {
        if (indegree[id] == 0)
        {
            ready.push(id);
        }
    }

    uint32_t visited = 0;
    while (!ready.empty())
    {
        ProtocolTransactionNodeId id = ready.front();
        ready.pop();
        visited++;
        for (ProtocolTransactionNodeId successor : successors[id])
        {
            if (--indegree[successor] == 0)
            {
                ready.push(successor);
            }
        }
    }
    if (visited != m_nodes.size())
    {
        return fail("transaction graph contains a dependency cycle");
    }

    std::vector<bool> reachesCompletion(m_nodes.size(), false);
    std::queue<ProtocolTransactionNodeId> reverseReady;
    reachesCompletion[completionId] = true;
    reverseReady.push(completionId);
    while (!reverseReady.empty())
    {
        ProtocolTransactionNodeId id = reverseReady.front();
        reverseReady.pop();
        for (ProtocolTransactionNodeId dependency : m_nodes[id].dependencies)
        {
            if (!reachesCompletion[dependency])
            {
                reachesCompletion[dependency] = true;
                reverseReady.push(dependency);
            }
        }
    }
    if (std::find(reachesCompletion.begin(), reachesCompletion.end(), false) !=
        reachesCompletion.end())
    {
        return fail("transaction graph contains a node that cannot affect completion");
    }

    return true;
}

TypeId
ProtocolTransactionExecutor::GetTypeId()
{
    static TypeId tid = TypeId("ns3::ProtocolTransactionExecutor")
                            .SetParent<Object>()
                            .SetGroupName("GpuCluster")
                            .AddConstructor<ProtocolTransactionExecutor>();
    return tid;
}

ProtocolTransactionExecutor::ProtocolTransactionExecutor() = default;

ProtocolTransactionExecutor::~ProtocolTransactionExecutor() = default;

bool
ProtocolTransactionExecutor::SetGraph(const ProtocolTransactionGraph& graph, std::string* error)
{
    if (m_started)
    {
        if (error)
        {
            *error = "cannot replace a running transaction graph";
        }
        return false;
    }
    if (!graph.Validate(error))
    {
        return false;
    }
    m_graph = graph;
    m_graphSet = true;
    m_successors.assign(m_graph.GetNodeCount(), {});
    for (const auto& node : m_graph.GetNodes())
    {
        for (ProtocolTransactionNodeId dependency : node.dependencies)
        {
            m_successors[dependency].push_back(node.id);
        }
    }
    InitializeRuntime();
    return true;
}

void
ProtocolTransactionExecutor::SetActionCallback(ActionCallback callback)
{
    m_actionCallback = std::move(callback);
}

void
ProtocolTransactionExecutor::SetNodeCompletionCallback(NodeCompletionCallback callback)
{
    m_nodeCompletionCallback = std::move(callback);
}

void
ProtocolTransactionExecutor::SetCompletionCallback(CompletionCallback callback)
{
    m_completionCallback = std::move(callback);
}

void
ProtocolTransactionExecutor::Start()
{
    NS_ASSERT_MSG(m_graphSet, "SetGraph must be called before starting a transaction");
    NS_ASSERT_MSG(!m_started, "Protocol transaction already started");
    m_started = true;
    for (const auto& node : m_graph.GetNodes())
    {
        if (node.dependencies.empty())
        {
            m_runtime[node.id].queued = true;
            m_readyNodes.push(node.id);
        }
    }
    Advance();
}

void
ProtocolTransactionExecutor::NotifyEvent(const ProtocolTransactionEvent& event)
{
    if (!m_started || m_complete)
    {
        return;
    }

    constexpr uint8_t SOURCE_WILDCARD = 1u << 0;
    constexpr uint8_t DESTINATION_WILDCARD = 1u << 1;
    constexpr uint8_t FLOW_WILDCARD = 1u << 2;
    constexpr uint8_t STAGE_WILDCARD = 1u << 3;
    constexpr uint8_t PACKET_TYPE_WILDCARD = 1u << 4;

    std::vector<ProtocolTransactionNodeId> completed;
    for (uint8_t wildcardMask = 0; wildcardMask < (1u << 5); ++wildcardMask)
    {
        WaitIndexKey key;
        key.eventType = static_cast<uint8_t>(event.type);
        key.sourceRank = (wildcardMask & SOURCE_WILDCARD) ? 0 : event.sourceRank;
        key.destinationRank =
            (wildcardMask & DESTINATION_WILDCARD) ? 0 : event.destinationRank;
        key.flowId = (wildcardMask & FLOW_WILDCARD) ? 0 : event.flowId;
        key.stageId = (wildcardMask & STAGE_WILDCARD) ? 0 : event.stageId;
        key.packetType = (wildcardMask & PACKET_TYPE_WILDCARD)
                             ? 0
                             : static_cast<uint8_t>(event.packetType);
        key.wildcardMask = wildcardMask;

        auto indexed = m_waitIndex.find(key);
        if (indexed == m_waitIndex.end())
        {
            continue;
        }

        for (ProtocolTransactionNodeId id : indexed->second)
        {
            const auto& node = m_graph.GetNode(id);
            RuntimeNode& runtime = m_runtime[id];
            if (runtime.state != ProtocolTransactionNodeState::ACTIVE ||
                !node.matcher.Matches(event))
            {
                continue;
            }

            if (std::numeric_limits<uint64_t>::max() - runtime.observedBytes < event.bytes)
            {
                runtime.observedBytes = std::numeric_limits<uint64_t>::max();
            }
            else
            {
                runtime.observedBytes += event.bytes;
            }
            runtime.observedEvents++;

            bool bytesSatisfied =
                node.targetBytes == 0 || runtime.observedBytes >= node.targetBytes;
            bool eventsSatisfied =
                node.targetEvents == 0 || runtime.observedEvents >= node.targetEvents;
            if (bytesSatisfied && eventsSatisfied)
            {
                completed.push_back(id);
            }
        }
    }

    for (ProtocolTransactionNodeId id : completed)
    {
        MarkCompleted(id);
    }
    if (!completed.empty())
    {
        Advance();
    }
}

void
ProtocolTransactionExecutor::Reset()
{
    for (auto& runtime : m_runtime)
    {
        if (runtime.delayEvent.IsPending())
        {
            Simulator::Cancel(runtime.delayEvent);
        }
    }
    InitializeRuntime();
    m_started = false;
    m_complete = false;
    m_completionNotified = false;
    m_advancing = false;
    m_advanceRequested = false;
}

bool
ProtocolTransactionExecutor::IsStarted() const
{
    return m_started;
}

bool
ProtocolTransactionExecutor::IsComplete() const
{
    return m_complete;
}

ProtocolTransactionNodeState
ProtocolTransactionExecutor::GetNodeState(ProtocolTransactionNodeId id) const
{
    NS_ASSERT_MSG(id < m_runtime.size(), "Protocol transaction node ID out of range");
    return m_runtime[id].state;
}

uint64_t
ProtocolTransactionExecutor::GetObservedBytes(ProtocolTransactionNodeId id) const
{
    NS_ASSERT_MSG(id < m_runtime.size(), "Protocol transaction node ID out of range");
    return m_runtime[id].observedBytes;
}

uint32_t
ProtocolTransactionExecutor::GetObservedEvents(ProtocolTransactionNodeId id) const
{
    NS_ASSERT_MSG(id < m_runtime.size(), "Protocol transaction node ID out of range");
    return m_runtime[id].observedEvents;
}

void
ProtocolTransactionExecutor::DoDispose()
{
    Reset();
    m_actionCallback = nullptr;
    m_nodeCompletionCallback = nullptr;
    m_completionCallback = nullptr;
    Object::DoDispose();
}

void
ProtocolTransactionExecutor::InitializeRuntime()
{
    m_runtime.assign(m_graph.GetNodeCount(), RuntimeNode{});
    for (const auto& node : m_graph.GetNodes())
    {
        m_runtime[node.id].remainingDependencies = node.dependencies.size();
    }
    m_waitIndex.clear();
    std::queue<ProtocolTransactionNodeId> empty;
    m_readyNodes.swap(empty);
}

bool
ProtocolTransactionExecutor::WaitIndexKey::operator==(const WaitIndexKey& other) const
{
    return eventType == other.eventType && sourceRank == other.sourceRank &&
           destinationRank == other.destinationRank && flowId == other.flowId &&
           stageId == other.stageId && packetType == other.packetType &&
           wildcardMask == other.wildcardMask;
}

std::size_t
ProtocolTransactionExecutor::WaitIndexKeyHash::operator()(const WaitIndexKey& key) const
{
    std::size_t seed = 0;
    auto combine = [&seed](auto value) {
        using Value = decltype(value);
        seed ^= std::hash<Value>{}(value) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
    };
    combine(key.eventType);
    combine(key.sourceRank);
    combine(key.destinationRank);
    combine(key.flowId);
    combine(key.stageId);
    combine(key.packetType);
    combine(key.wildcardMask);
    return seed;
}

ProtocolTransactionExecutor::WaitIndexKey
ProtocolTransactionExecutor::MakeWaitIndexKey(const ProtocolTransactionEventMatcher& matcher)
{
    constexpr uint8_t SOURCE_WILDCARD = 1u << 0;
    constexpr uint8_t DESTINATION_WILDCARD = 1u << 1;
    constexpr uint8_t FLOW_WILDCARD = 1u << 2;
    constexpr uint8_t STAGE_WILDCARD = 1u << 3;
    constexpr uint8_t PACKET_TYPE_WILDCARD = 1u << 4;

    WaitIndexKey key;
    key.eventType = static_cast<uint8_t>(matcher.type);
    if (matcher.sourceRank == PROTOCOL_TRANSACTION_ANY_RANK)
    {
        key.wildcardMask |= SOURCE_WILDCARD;
    }
    else
    {
        key.sourceRank = matcher.sourceRank;
    }
    if (matcher.destinationRank == PROTOCOL_TRANSACTION_ANY_RANK)
    {
        key.wildcardMask |= DESTINATION_WILDCARD;
    }
    else
    {
        key.destinationRank = matcher.destinationRank;
    }
    if (matcher.flowId == PROTOCOL_TRANSACTION_ANY_FLOW)
    {
        key.wildcardMask |= FLOW_WILDCARD;
    }
    else
    {
        key.flowId = matcher.flowId;
    }
    if (matcher.stageId == std::numeric_limits<uint32_t>::max())
    {
        key.wildcardMask |= STAGE_WILDCARD;
    }
    else
    {
        key.stageId = matcher.stageId;
    }
    if (!matcher.matchPacketType)
    {
        key.wildcardMask |= PACKET_TYPE_WILDCARD;
    }
    else
    {
        key.packetType = static_cast<uint8_t>(matcher.packetType);
    }
    return key;
}

void
ProtocolTransactionExecutor::RegisterWait(const ProtocolTransactionNode& node)
{
    m_waitIndex[MakeWaitIndexKey(node.matcher)].push_back(node.id);
}

void
ProtocolTransactionExecutor::PropagateCompletion(ProtocolTransactionNodeId id)
{
    for (ProtocolTransactionNodeId successorId : m_successors[id])
    {
        RuntimeNode& successor = m_runtime[successorId];
        if (successor.state != ProtocolTransactionNodeState::PENDING || successor.queued)
        {
            continue;
        }

        const auto& successorNode = m_graph.GetNode(successorId);
        if (successorNode.dependency == ProtocolTransactionDependency::ANY)
        {
            successor.queued = true;
            m_readyNodes.push(successorId);
            continue;
        }

        NS_ASSERT(successor.remainingDependencies > 0);
        successor.remainingDependencies--;
        if (successor.remainingDependencies == 0)
        {
            successor.queued = true;
            m_readyNodes.push(successorId);
        }
    }
}

void
ProtocolTransactionExecutor::Advance()
{
    if (m_advancing)
    {
        m_advanceRequested = true;
        return;
    }

    m_advancing = true;
    do
    {
        m_advanceRequested = false;
        std::vector<ProtocolTransactionAction> actions;
        while (!m_readyNodes.empty())
        {
            ProtocolTransactionNodeId id = m_readyNodes.front();
            m_readyNodes.pop();
            RuntimeNode& runtime = m_runtime[id];
            runtime.queued = false;
            if (runtime.state != ProtocolTransactionNodeState::PENDING)
            {
                continue;
            }

            const auto& node = m_graph.GetNode(id);
            switch (node.type)
            {
            case ProtocolTransactionNodeType::ACTION:
                actions.push_back(node.action);
                MarkCompleted(id);
                break;
            case ProtocolTransactionNodeType::WAIT:
                runtime.state = ProtocolTransactionNodeState::ACTIVE;
                RegisterWait(node);
                break;
            case ProtocolTransactionNodeType::DELAY:
                runtime.state = ProtocolTransactionNodeState::ACTIVE;
                runtime.delayEvent = Simulator::Schedule(
                    node.delay,
                    &ProtocolTransactionExecutor::OnDelayExpired,
                    this,
                    id);
                break;
            case ProtocolTransactionNodeType::COMPLETE:
                MarkCompleted(id);
                m_complete = true;
                break;
            }
        }

        for (const auto& action : actions)
        {
            if (m_actionCallback)
            {
                m_actionCallback(action);
            }
        }
    } while (m_advanceRequested);
    m_advancing = false;
    MaybeNotifyCompletion();
}

void
ProtocolTransactionExecutor::OnDelayExpired(ProtocolTransactionNodeId id)
{
    if (!m_started || m_complete || id >= m_runtime.size())
    {
        return;
    }
    if (m_runtime[id].state != ProtocolTransactionNodeState::ACTIVE)
    {
        return;
    }
    MarkCompleted(id);
    Advance();
}

void
ProtocolTransactionExecutor::MarkCompleted(ProtocolTransactionNodeId id)
{
    RuntimeNode& runtime = m_runtime[id];
    if (runtime.state == ProtocolTransactionNodeState::COMPLETED)
    {
        return;
    }
    runtime.state = ProtocolTransactionNodeState::COMPLETED;
    if (m_nodeCompletionCallback)
    {
        m_nodeCompletionCallback(id);
    }
    PropagateCompletion(id);
}

void
ProtocolTransactionExecutor::MaybeNotifyCompletion()
{
    if (m_complete && !m_completionNotified)
    {
        m_completionNotified = true;
        if (m_completionCallback)
        {
            m_completionCallback();
        }
    }
}

} // namespace ns3
