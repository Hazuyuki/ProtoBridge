/* SPDX-License-Identifier: GPL-2.0-only */
#include "rdma-operation-traffic.h"

#include "ns3/abort.h"
#include "ns3/simulator.h"

#include <algorithm>

namespace ns3
{
NS_OBJECT_ENSURE_REGISTERED(RdmaOperationTraffic);

TypeId
RdmaOperationTraffic::GetTypeId()
{
    static TypeId tid = TypeId("ns3::RdmaOperationTraffic")
                            .SetParent<Object>()
                            .SetGroupName("GpuCluster")
                            .AddConstructor<RdmaOperationTraffic>();
    return tid;
}

void
RdmaOperationTraffic::Initialize(Ptr<FabricEndpoint> source,
                                 Ptr<FabricEndpoint> peer,
                                 uint64_t bytes,
                                 uint32_t iterations,
                                 const RdmaConfig& config)
{
    NS_ABORT_MSG_IF(!source || !peer || !source->GetRdmaOperationEngine() ||
                        !peer->GetRdmaOperationEngine() || source->IsLocalRank(peer->GetRank()) ||
                        !iterations,
                    "RDMA workload requires detailed mode and a remote peer");
    m_source = source;
    m_peer = peer;
    m_config = config;
    m_opcode = ParseRdmaOpcode(config.operation);
    m_bytes = RdmaUsesReadAtomicCredit(m_opcode) && m_opcode != RdmaOpcode::READ ? 8 : bytes;
    NS_ABORT_MSG_IF(!m_bytes, "RDMA operation payload must be nonzero");
    m_iterations = iterations;
    NS_ABORT_MSG_IF(m_bytes > UINT64_MAX - std::max(config.localAddress, config.remoteAddress) ||
                        config.signalAddress > UINT64_MAX - 8,
                    "RDMA workload memory range overflows");
    uint64_t size = std::max(std::max(config.localAddress, config.remoteAddress) + m_bytes,
                             config.signalAddress + 8);
    m_source->RegisterRdmaMemory(0, size);
    m_peer->RegisterRdmaMemory(0, size);
    auto src = m_source->GetRdmaOperationEngine(), dst = m_peer->GetRdmaOperationEngine();
    if (m_opcode == RdmaOpcode::FETCH_ADD || m_opcode == RdmaOpcode::COMPARE_SWAP)
    {
        dst->WriteWord(config.remoteAddress, config.initialValue);
    }
    else if (m_opcode != RdmaOpcode::SIGNAL)
    {
        std::vector<uint8_t> pattern(static_cast<size_t>(std::min<uint64_t>(8, m_bytes)));
        for (uint32_t i = 0; i < pattern.size(); ++i)
        {
            pattern[i] = i + 1;
        }
        if (m_opcode == RdmaOpcode::READ)
        {
            dst->WriteMemory(config.remoteAddress, pattern);
        }
        else
        {
            src->WriteMemory(config.localAddress, pattern);
        }
    }
    if (RdmaNeedsReceive(m_opcode))
    {
        // Round 63 (codex finding 17): a zero staging chunk divides by zero
        // in the staged chunk count below.
        NS_ABORT_MSG_IF(config.dataPath == "staged" && config.stagingChunkBytes == 0,
                        "RDMA staged data path requires a nonzero staging chunk size");
        uint64_t chunks =
            config.dataPath == "staged" ? 1 + (m_bytes - 1) / config.stagingChunkBytes : 1;
        NS_ABORT_MSG_IF(chunks > UINT64_MAX / iterations, "RDMA receive count overflows");
        m_expectedReceives = chunks * iterations;
    }
    if (m_opcode == RdmaOpcode::PUT_SIGNAL || m_opcode == RdmaOpcode::SIGNAL)
    {
        dst->WriteWord(config.signalAddress, 0);
    }
}

void
RdmaOperationTraffic::SetCompletionCallback(std::function<void(bool, uint64_t)> callback)
{
    m_complete = std::move(callback);
}

void
RdmaOperationTraffic::PostNextReceive()
{
    if (m_posted >= m_expectedReceives || m_done)
    {
        return;
    }
    uint64_t capacity = m_config.dataPath == "staged"
                            ? std::min<uint64_t>(m_bytes, m_config.stagingChunkBytes)
                            : m_bytes;
    if (m_opcode == RdmaOpcode::WRITE_IMM)
    {
        capacity = 0;
    }
    uint64_t id = m_posted++;
    m_peer->PostRdmaReceive(
        id,
        m_config.remoteAddress,
        capacity,
        [this](const RdmaReceiveCompletion& result) {
            ++m_received;
            PostNextReceive();
            TryComplete();
        },
        m_source->GetRank());
}

void
RdmaOperationTraffic::Start()
{
    m_startNs = Simulator::Now().GetNanoSeconds();
    if (m_expectedReceives)
    {
        m_postEvent = Simulator::Schedule(m_config.receivePostDelay, [this] {
            uint64_t count = std::min<uint64_t>(m_config.receiveQueueDepth, m_expectedReceives);
            for (uint64_t i = 0; i < count; ++i)
            {
                PostNextReceive();
            }
        });
    }
    if (m_opcode == RdmaOpcode::PUT_SIGNAL || m_opcode == RdmaOpcode::SIGNAL)
    {
        m_signalReady = false;
        uint64_t expected = m_config.signalOperation == "set" ? m_config.signalValue
                                                              : m_config.signalValue * m_iterations;
        m_peer->GetRdmaOperationEngine()->WaitSignal(m_config.signalAddress,
                                                     RdmaComparison::EQ,
                                                     expected,
                                                     [this](uint64_t) {
                                                         m_signalReady = true;
                                                         TryComplete();
                                                     });
    }
    for (uint32_t i = 0; i < m_iterations; ++i)
    {
        RdmaRequest request;
        request.opcode = m_opcode;
        request.peer = m_peer->GetRank();
        request.bytes = m_bytes;
        request.localAddress = m_config.localAddress;
        request.remoteAddress = m_config.remoteAddress;
        request.operand = m_config.operand;
        request.compare = m_config.compare;
        request.immediate = m_config.immediate;
        request.signalAddress = m_config.signalAddress;
        request.signalValue = m_config.signalValue;
        request.signalOp =
            m_config.signalOperation == "set" ? RdmaSignalOp::SET : RdmaSignalOp::ADD;
        m_source->SubmitRdmaOperation(request, [this](const RdmaCompletion& result) {
            if (result.success)
            {
                ++m_completed;
            }
            else
            {
                ++m_failed;
            }
            m_lastValue = result.value;
            TryComplete();
        });
    }
}

void
RdmaOperationTraffic::TryComplete()
{
    if (m_done || m_completed + m_failed != m_iterations)
    {
        return;
    }
    if (!m_failed && (m_received != m_expectedReceives || !m_signalReady))
    {
        return;
    }
    m_done = true;
    if (m_complete)
    {
        m_complete(!m_failed, Simulator::Now().GetNanoSeconds() - m_startNs);
    }
}

uint64_t
RdmaOperationTraffic::GetCompleted() const
{
    return m_completed;
}

uint64_t
RdmaOperationTraffic::GetFailed() const
{
    return m_failed;
}

uint64_t
RdmaOperationTraffic::GetLastValue() const
{
    return m_lastValue;
}

uint64_t
RdmaOperationTraffic::GetSignalValue() const
{
    return m_peer->GetRdmaOperationEngine()->ReadWord(m_config.signalAddress);
}

uint64_t
RdmaOperationTraffic::GetRemoteValue() const
{
    if (m_config.remoteAddress % 8 || m_bytes < 8)
    {
        return 0;
    }
    return m_peer->GetRdmaOperationEngine()->ReadWord(m_config.remoteAddress);
}

void
RdmaOperationTraffic::DoDispose()
{
    if (m_postEvent.IsPending())
    {
        Simulator::Cancel(m_postEvent);
    }
    m_complete = {};
    m_source = nullptr;
    m_peer = nullptr;
    Object::DoDispose();
}
} // namespace ns3
