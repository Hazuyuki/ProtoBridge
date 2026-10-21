/*
 * SPDX-License-Identifier: GPL-2.0-only
 * ring-allreduce.cc
 *
 * Pipelined Ring AllReduce Coordinator with protocol-aware sending.
 * Uses SendDataAutoProtocol to include NCCL protocol wire overhead.
 *
 * Pipeline: each GPU independently advances through steps upon
 * receiving its segment. No global barrier between steps.
 *
 * Reduce-Scatter steps (0..N-2): GPU i sends to (i+1)%N clockwise
 * AllGather steps (N-1..2N-3): GPU i sends to (i-1+N)%N counterclockwise
 */

#include "ring-allreduce.h"

#include "nccl-protocol.h"
#include "protocol-model.h"

#include "ns3/boolean.h"
#include "ns3/fatal-error.h"
#include "ns3/log.h"
#include "ns3/simulator.h"

#include <limits>

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("RingAllReduce");

NS_OBJECT_ENSURE_REGISTERED(RingAllReduce);

TypeId
RingAllReduce::GetTypeId()
{
    static TypeId tid = TypeId("ns3::RingAllReduce")
                            .SetParent<CollectiveInjector>()
                            .SetGroupName("GpuCluster")
                            .AddConstructor<RingAllReduce>()
                            .AddAttribute("PerStepComputeDelay",
                                "Software overhead per Reduce-Scatter step in nanoseconds.",
                                UintegerValue(0),
                                MakeUintegerAccessor(&RingAllReduce::m_perStepSwOverheadNs),
                                MakeUintegerChecker<uint64_t>(0))
                            .AddAttribute("UseTransactionModel",
                                "Use the event-driven protocol transaction model.",
                                BooleanValue(true),
                                MakeBooleanAccessor(&RingAllReduce::m_useTransactionModel),
                                MakeBooleanChecker());
    return tid;
}

RingAllReduce::RingAllReduce()
    : m_numGpus(0),
      m_dataSize(0),
      m_segmentSize(0),
      m_flowId(1),
      m_protocolId(0),
      m_useTransactionModel(true),
      m_state(RingState::IDLE),
      m_startupDelayNs(0),
      m_perStepSwOverheadNs(0),
      m_swOverheadPerByteNs(0),
      m_computeBaseLLNs(0),
      m_computeBaseLL128Ns(0),
      m_computeBaseSIMPLENs(0),
      m_llThreshold(8192),
      m_ll128Threshold(2 * 1024 * 1024),
      m_chunkSize(0),
      m_segmentRemainder(0),
      m_sentDataBytes(0),
      m_gpuDoneCount(0),
      m_permanentLossCount(0),
      m_reduceScatterSteps(0),
      m_allGatherSteps(0),
      m_totalSteps(0),
      m_startTimeNs(0),
      m_endTimeNs(0)
{
}

RingAllReduce::~RingAllReduce()
{
}

void
RingAllReduce::DoDispose()
{
    if (m_transactionExecutor)
    {
        m_transactionExecutor->Reset();
        m_transactionExecutor->SetActionCallback({});
        m_transactionExecutor->SetNodeCompletionCallback({});
        m_transactionExecutor->SetCompletionCallback({});
        m_transactionExecutor = nullptr;
    }
    m_transactionGraph.Clear();
    m_transactionGpuByNode.clear();
    m_transactionStepByNode.clear();
    m_endpoints.clear();
    m_completionCallback = nullptr;
    CollectiveInjector::DoDispose();
}

void
RingAllReduce::Initialize(uint16_t numGpus, uint64_t dataSize,
                          const std::vector<Ptr<FabricEndpoint>>& endpoints)
{
    m_numGpus = numGpus;
    m_dataSize = dataSize;
    m_endpoints = endpoints;

    // Segment grid: rank r's base segment is dataSize/numGpus bytes, with the
    // first dataSize % numGpus segments carrying one extra byte, so the grid
    // sums to exactly dataSize (codex round 13, finding 3: plain integer
    // division silently dropped dataSize % numGpus bytes — 4 ranks / 15 B
    // reported complete after 72 effective bytes where 2(N-1)*D = 90).
    m_segmentSize = dataSize / numGpus;
    m_segmentRemainder = dataSize % numGpus;
    if (m_segmentSize == 0)
    {
        // Degenerate dataSize < numGpus: keep the legacy fallback (each rank
        // transmits the whole dataSize every step) — an empty-segment grid
        // would need zero-byte steps the receive ledger cannot close.
        m_segmentSize = dataSize;
        m_segmentRemainder = 0;
    }
    m_chunkSize = m_segmentSize;

    m_reduceScatterSteps = numGpus - 1;
    m_allGatherSteps = numGpus - 1;
    m_totalSteps = 2 * (numGpus - 1);

    m_gpuCompletedStep.resize(numGpus, 0);
    m_gpuStepReceivedBytes.assign(numGpus,
                                  std::vector<uint64_t>(m_totalSteps, 0));
    m_gpuDoneCount = 0;

    m_flowId = 1;

    // Protocol selection: startup delay based on total data size (collective launch cost),
    // wire protocol based on per-step segment size (per-channel payload)
    Ptr<ProtocolModel> protoModel = m_endpoints[0]->GetProtocolModel();
    NcclProtocol startupProto = static_cast<NcclProtocol>(protoModel->GetProtocolId(m_dataSize));
    if (m_startupDelayNs == 0)
    {
        m_startupDelayNs = protoModel->GetStartupDelayNs(static_cast<uint8_t>(startupProto), m_numGpus);
    }
    // Wire protocol: segment size determines per-channel protocol (LL/LL128/SIMPLE)
    NcclProtocol wireProto = static_cast<NcclProtocol>(protoModel->GetProtocolId(m_segmentSize));
    m_protocolId = static_cast<uint8_t>(wireProto);

    SetupReceiveCallbacks();

    NS_LOG_INFO("Pipelined RingAllReduce initialized: numGpus=" << numGpus
                << " dataSize=" << dataSize << " segmentSize=" << m_segmentSize
                << " totalSteps=" << m_totalSteps
                << " protocol=" << static_cast<int>(wireProto)
                << " startupDelay=" << m_startupDelayNs << "ns");
}

void
RingAllReduce::SetupReceiveCallbacks()
{
    for (auto& ep : m_endpoints)
    {
        if (ep)
        {
            ep->SetReceiveCallback(
                MakeCallback(&RingAllReduce::OnPacketReceived, this));
        }
    }
}

void
RingAllReduce::SetCompletionCallback(std::function<void(uint64_t durationNs)> cb)
{
    m_completionCallback = cb;
}

void
RingAllReduce::SetStartupDelay(Time delay)
{
    m_startupDelayNs = delay.GetNanoSeconds();
}

void
RingAllReduce::SetPerStepSwOverhead(Time delay)
{
    m_perStepSwOverheadNs = delay.GetNanoSeconds();
}

void RingAllReduce::SetSwOverheadPerByteNs(uint64_t perByteNs) { m_swOverheadPerByteNs = perByteNs; }
void RingAllReduce::SetLlThreshold(uint64_t threshold) { m_llThreshold = threshold; }
void RingAllReduce::SetLl128Threshold(uint64_t threshold) { m_ll128Threshold = threshold; }
void RingAllReduce::SetComputeBaseDelays(Time ll, Time ll128, Time simple)
{
    m_computeBaseLLNs = ll.GetNanoSeconds();
    m_computeBaseLL128Ns = ll128.GetNanoSeconds();
    m_computeBaseSIMPLENs = simple.GetNanoSeconds();
}
uint64_t RingAllReduce::GetPerStepSwOverheadNs(uint64_t chunkSize) const
{
    if (m_swOverheadPerByteNs > 0 ||
        m_computeBaseLLNs > 0 || m_computeBaseLL128Ns > 0 || m_computeBaseSIMPLENs > 0)
    {
        uint64_t baseDelay;
        if (chunkSize < m_llThreshold)
            baseDelay = m_computeBaseLLNs;
        else if (chunkSize < m_ll128Threshold)
            baseDelay = m_computeBaseLL128Ns;
        else
            baseDelay = m_computeBaseSIMPLENs;
        return baseDelay + m_swOverheadPerByteNs * chunkSize;
    }
    return m_perStepSwOverheadNs;
}

void
RingAllReduce::Start()
{
    m_state = RingState::RUNNING;
    m_startTimeNs = Simulator::Now().GetNanoSeconds();

    NS_LOG_INFO("Starting Pipelined Ring AllReduce with startup delay " << m_startupDelayNs << " ns");

    if (m_useTransactionModel)
    {
        BuildTransactionGraph();
        m_transactionExecutor->Start();
        return;
    }

    Simulator::Schedule(NanoSeconds(m_startupDelayNs), &RingAllReduce::TriggerInitialSends, this);
}

void
RingAllReduce::BuildTransactionGraph()
{
    m_transactionGraph.Clear();
    m_transactionGpuByNode.clear();
    m_transactionStepByNode.clear();

    const auto startup = m_transactionGraph.AddDelay(NanoSeconds(m_startupDelayNs),
                                                     {},
                                                     "collective-startup");
    std::vector<ProtocolTransactionNodeId> finalSteps;
    finalSteps.reserve(m_numGpus);

    Ptr<ProtocolModel> protocolModel = m_endpoints[0]->GetProtocolModel();
    const uint16_t invalidGpu = std::numeric_limits<uint16_t>::max();
    const uint32_t invalidStep = std::numeric_limits<uint32_t>::max();

    // A ring step is a permutation: every GPU receives exactly one transfer
    // per step (the send whose GetSendTarget IS that GPU). The transaction
    // graph must gate each GPU's step s+1 send on DELIVERY of its step-s
    // input — the same receive-driven rule the wire path applies in
    // OnGpuStepReceiveComplete — not on delivery of the GPU's own outgoing
    // step-s transfer (codex round 15, finding 3: the old gpu-major loop
    // chained dependency = own previous wait node, so a rank transmitted at
    // wire speed regardless of input arrival — with three 1-us links and one
    // 100-us link, transaction and receive-driven runs disagreed by ~100 us
    // per slow-link traversal). Step-major construction keeps every input
    // wait node of step s-1 available when step s's sends are wired.
    std::vector<ProtocolTransactionNodeId> sendDependency(m_numGpus, startup);
    const uint64_t swOverheadNs = GetPerStepSwOverheadNs(m_chunkSize);

    for (uint32_t step = 0; step < m_totalSteps; ++step)
    {
        // Wait node of the transfer DELIVERED to each GPU at this step
        // (startUp sentinel for GPUs no transfer targets — a broken ring
        // embedding — degrades to fire-immediately rather than deadlock).
        std::vector<ProtocolTransactionNodeId> deliveredTo(m_numGpus, startup);
        for (uint16_t gpu = 0; gpu < m_numGpus; ++gpu)
        {
            ProtocolTransactionRequest request;
            request.kind = ProtocolTransactionKind::DATA_TRANSFER;
            request.packetType = FabricPacketType::DATA;
            request.sourceRank = gpu;
            request.destinationRank = GetSendTarget(gpu, step);
            request.flowId = m_flowId + step;
            request.virtualChannel = 0;
            request.effectiveBytes = SegmentSizeOf(gpu, step);
            request.stageId = step;
            request.hasProtocolId = true;
            request.protocolId = m_protocolId;
            request.label = "gpu-" + std::to_string(gpu) + "-step-" + std::to_string(step);

            const auto wait =
                protocolModel->AddTransaction(m_transactionGraph, request, {sendDependency[gpu]});
            if (m_transactionGpuByNode.size() <= wait)
            {
                m_transactionGpuByNode.resize(wait + 1, invalidGpu);
                m_transactionStepByNode.resize(wait + 1, invalidStep);
            }
            // The wait node tracks delivery TO the destination rank: node
            // completion means that RECEIVER finished the step (the wire
            // path's LogStepComplete(receiver, ...) semantics).
            m_transactionGpuByNode[wait] = request.destinationRank;
            m_transactionStepByNode[wait] = step;
            if (request.destinationRank < m_numGpus)
            {
                deliveredTo[request.destinationRank] = wait;
            }
        }

        for (uint16_t gpu = 0; gpu < m_numGpus; ++gpu)
        {
            if (step + 1 < m_totalSteps)
            {
                // Post-receive software overhead before this GPU's next
                // send, mirroring OnGpuStepReceiveComplete's scheduled
                // SendGpuStepData(gpu, step + 1).
                sendDependency[gpu] = m_transactionGraph.AddDelay(
                    NanoSeconds(swOverheadNs),
                    {deliveredTo[gpu]},
                    "gpu-" + std::to_string(gpu) + "-step-delay");
            }
            else
            {
                // A GPU is done when its LAST step's input is delivered —
                // the same condition the wire path counts in
                // OnGpuStepReceiveComplete.
                finalSteps.push_back(deliveredTo[gpu]);
            }
        }
    }
    m_transactionGraph.AddCompletion(finalSteps);
    m_transactionGpuByNode.resize(m_transactionGraph.GetNodeCount(), invalidGpu);
    m_transactionStepByNode.resize(m_transactionGraph.GetNodeCount(), invalidStep);

    m_transactionExecutor = CreateObject<ProtocolTransactionExecutor>();
    m_transactionExecutor->SetActionCallback(
        [this](const ProtocolTransactionAction& action) { ExecuteTransactionAction(action); });
    m_transactionExecutor->SetNodeCompletionCallback(
        [this](ProtocolTransactionNodeId nodeId) { OnTransactionNodeComplete(nodeId); });
    m_transactionExecutor->SetCompletionCallback([this]() { OnTransactionComplete(); });

    std::string error;
    if (!m_transactionExecutor->SetGraph(m_transactionGraph, &error))
    {
        NS_FATAL_ERROR("Invalid Ring AllReduce transaction graph: " << error);
    }
}

void
RingAllReduce::ExecuteTransactionAction(const ProtocolTransactionAction& action)
{
    if (action.type != ProtocolTransactionActionType::SEND_DATA ||
        action.sourceRank >= m_endpoints.size() || !m_endpoints[action.sourceRank])
    {
        NS_FATAL_ERROR("Ring AllReduce emitted an invalid transaction action");
    }
    m_endpoints[action.sourceRank]->SendBulkWireTransferSize(
        action.destinationRank,
        action.effectiveBytes,
        action.protocolId,
        action.flowId,
        action.virtualChannel);
}

void
RingAllReduce::OnTransactionNodeComplete(ProtocolTransactionNodeId nodeId)
{
    if (nodeId >= m_transactionGpuByNode.size())
    {
        return;
    }

    const uint16_t gpu = m_transactionGpuByNode[nodeId];
    const uint32_t step = m_transactionStepByNode[nodeId];
    if (gpu >= m_numGpus || step >= m_totalSteps)
    {
        return;
    }

    m_gpuCompletedStep[gpu] = step + 1;
    LogStepComplete(gpu, step, m_startTimeNs, Simulator::Now().GetNanoSeconds());
}

void
RingAllReduce::OnTransactionComplete()
{
    m_gpuDoneCount = m_numGpus;
    Complete();
}

void
RingAllReduce::TriggerInitialSends()
{
    // All GPUs simultaneously send their step-0 segment
    for (uint16_t gpu = 0; gpu < m_numGpus; gpu++)
    {
        SendGpuStepData(gpu, 0);
    }
}

bool
RingAllReduce::IsReduceScatterStep(uint32_t step) const
{
    return step < m_reduceScatterSteps;
}

bool
RingAllReduce::IsAllGatherStep(uint32_t step) const
{
    return step >= m_reduceScatterSteps && step < m_totalSteps;
}

uint16_t
RingAllReduce::GetSendTarget(uint16_t gpu, uint32_t step) const
{
    if (IsReduceScatterStep(step))
    {
        // Reduce-Scatter: clockwise. Prefer the topology-embedded next neighbor
        // (set by GpuClusterTopologyHelper) so ring steps are physically local;
        // fall back to rank arithmetic when no embedding is installed.
        if (gpu < m_endpoints.size() && m_endpoints[gpu])
        {
            uint16_t next = m_endpoints[gpu]->GetRingNext();
            if (next != 0xFFFF)
            {
                return next;
            }
        }
        return (gpu + 1) % m_numGpus;
    }
    else
    {
        // AllGather: counterclockwise.
        if (gpu < m_endpoints.size() && m_endpoints[gpu])
        {
            uint16_t prev = m_endpoints[gpu]->GetRingPrev();
            if (prev != 0xFFFF)
            {
                return prev;
            }
        }
        return (gpu - 1 + m_numGpus) % m_numGpus;
    }
}

uint64_t
RingAllReduce::SegmentSizeOf(uint16_t gpu, uint32_t step) const
{
    // Which segment the GPU holds at this step. Reduce-Scatter sends run
    // clockwise (to ring next): ownership of chunk c sits at rank
    // (c + s) mod N, so rank g holds chunk (g - s) mod N. The AllGather
    // phase REVERSES the send direction (counterclockwise, to ring prev),
    // so ownership keeps rotating in the SAME physical direction the bytes
    // flow: continuing (g - s) mod N across the reversal let the index
    // drift backwards and chunks changed size as they traveled (codex
    // round 14, finding 4: at 4 GPUs / 259 B, rank 1 received a 64-B
    // chunk at step 3 and forwarded 65 B at step 4). From the first AG
    // step s0 = N-1 the offset advances +1 per step instead:
    // idx = (g + s - 2(N-1)) mod N, which preserves the chunk invariant —
    // a rank forwards at step s+1 exactly the chunk it received at step s.
    // That expression simplifies algebraically to (g + s + 2) mod N; the
    // simplified form is what gets evaluated because the direct one wraps
    // in unsigned arithmetic (s - 2(N-1) is negative throughout AG, and
    // 2^32 mod N != 0 for N = 3 or 5, which silently shifted the index).
    // The base size is uniform with the first dataSize % numGpus segments
    // one byte larger.
    const uint32_t n = m_numGpus;
    uint32_t idx;
    if (step < n - 1)
    {
        idx = (gpu + n - (step % n)) % n;
    }
    else
    {
        idx = (gpu + step + 2) % n;
    }
    return m_segmentSize + (idx < m_segmentRemainder ? 1 : 0);
}

void
RingAllReduce::SendGpuStepData(uint16_t gpu, uint32_t step)
{
    if (gpu >= m_endpoints.size() || !m_endpoints[gpu])
    {
        return;
    }

    uint16_t target = GetSendTarget(gpu, step);
    uint64_t segmentBytes = SegmentSizeOf(gpu, step);
    m_sentDataBytes += segmentBytes;

    // Use a unique flowId per step to avoid reorder buffer confusion
    uint16_t stepFlowId = m_flowId + step;

    m_endpoints[gpu]->SendBulkWireTransferSize(
        target, segmentBytes, m_protocolId, stepFlowId, 0);

    NS_LOG_DEBUG("GPU " << gpu << " sending step " << step
                 << " to GPU " << target
                 << " (phase=" << (IsReduceScatterStep(step) ? "RS" : "AG")
                 << " flowId=" << stepFlowId << ")");
}

void
RingAllReduce::OnPacketReceived(uint16_t receiverRank, Ptr<Packet> packet, FabricHeader header)
{
    if (header.GetPacketType() != FabricPacketType::DATA
        && header.GetPacketType() != FabricPacketType::PERMANENT_LOSS)
    {
        return;
    }

    // The callback's first argument is the RECEIVING endpoint's rank; the
    // sender is in the header (the wire hops are single-switch, so the
    // original source rank survives unmodified).
    uint16_t srcRank = header.GetSourceRank();
    uint16_t destRank = receiverRank;

    uint32_t effectiveSize = header.GetEffectiveDataSize();
    if (effectiveSize == 0)
    {
        effectiveSize = packet->GetSize();
    }

    if (m_useTransactionModel)
    {
        if (header.GetPacketType() == FabricPacketType::PERMANENT_LOSS)
        {
            m_permanentLossCount++;
            return;
        }

        ProtocolTransactionEvent event;
        event.type = ProtocolTransactionEventType::PACKET_DELIVERED;
        event.packetType = header.GetPacketType();
        event.sourceRank = header.GetSourceRank();
        event.destinationRank = destRank;
        event.flowId = header.GetFlowId();
        event.bytes = effectiveSize;
        event.stageId = event.flowId >= m_flowId
                            ? static_cast<uint32_t>(event.flowId - m_flowId)
                            : std::numeric_limits<uint32_t>::max();
        m_transactionExecutor->NotifyEvent(event);
        return;
    }

    // Ignore late packets after this GPU has completed all steps
    if (m_gpuCompletedStep[destRank] >= m_totalSteps)
    {
        return;
    }

    // Accumulate effective bytes for this destination GPU's current step
    uint32_t currentStep = m_gpuCompletedStep[destRank];

    if (header.GetPacketType() == FabricPacketType::PERMANENT_LOSS)
    {
        // PERMANENT_LOSS means data integrity is violated — the packet was
        // lost after exhausting all retries. In a correctness-preserving model,
        // permanently lost data MUST NOT be counted toward collective progress.
        // The collective cannot produce a correct result with missing data.
        // Instead, record the loss and stall — no silent corruption.
        NS_LOG_INFO("PERMANENT_LOSS: src=" << srcRank << " -> dest=" << destRank
                     << " effectiveSize=" << effectiveSize
                     << " step=" << currentStep
                     << " (data integrity violated, collective cannot complete correctly)");
        m_permanentLossCount++;
        return;
    }

    // The step this packet belongs to is encoded in the flowId
    // (stepFlowId = m_flowId + step): the sender's step counter runs
    // independently of this receiver's currentStep in the pipeline — the
    // sender may already be a step ahead.
    const uint32_t senderStep = (header.GetFlowId() >= m_flowId)
                                    ? header.GetFlowId() - m_flowId
                                    : 0;

    // Per-STEP ledger, keyed by the SENDER's step (codex round 14,
    // finding 3). A single per-dest counter mixed bytes of different
    // steps whenever the pipelined senders drifted ahead of the
    // receiver, or a retransmission delivered a late copy after the
    // counter had already advanced — the mixed counter could cross one
    // step's threshold with another step's bytes and then starve a later
    // step, stalling the collective despite zero permanent losses.
    if (senderStep >= m_totalSteps)
    {
        return; // malformed/late flowId: ignore rather than corrupt ledgers
    }
    m_gpuStepReceivedBytes[destRank][senderStep] += effectiveSize;

    NS_LOG_DEBUG("Packet: src=" << srcRank << " -> dest=" << destRank
                     << " wireSize=" << packet->GetSize()
                     << " effectiveSize=" << effectiveSize
                     << " totalReceived=" << m_gpuStepReceivedBytes[destRank][senderStep]
                     << " step=" << currentStep);

    // A step is complete when its own ledger entry reaches the SENDER's
    // segment size at that step; steps complete in order (currentStep).
    while (m_gpuCompletedStep[destRank] < m_totalSteps &&
           m_gpuStepReceivedBytes[destRank][m_gpuCompletedStep[destRank]]
               >= StepExpectedBytes(destRank, m_gpuCompletedStep[destRank]))
    {
        // GPU completed receiving its segment for this step
        const uint32_t completedStep = m_gpuCompletedStep[destRank];
        m_gpuCompletedStep[destRank] = completedStep + 1;

        NS_LOG_INFO("GPU " << destRank << " completed step " << completedStep
                     << " (now at step " << m_gpuCompletedStep[destRank] << ")");

        OnGpuStepReceiveComplete(destRank, completedStep);
    }
}

uint64_t
RingAllReduce::StepExpectedBytes(uint16_t destRank, uint32_t step) const
{
    // Bytes destRank must receive at `step`: the segment the SENDER at
    // that step holds. The sender is the ring predecessor (RS clockwise
    // sends; AG counterclockwise sends), so it is the GPU whose step-step
    // send targets destRank.
    for (uint16_t gpu = 0; gpu < m_numGpus; ++gpu)
    {
        if (GetSendTarget(gpu, step) == destRank)
        {
            return SegmentSizeOf(gpu, step);
        }
    }
    return m_segmentSize;
}

void
RingAllReduce::OnGpuStepReceiveComplete(uint16_t gpu, uint32_t step)
{
    LogStepComplete(gpu, step, m_startTimeNs, Simulator::Now().GetNanoSeconds());

    if (m_gpuCompletedStep[gpu] >= m_totalSteps)
    {
        // This GPU has completed all steps
        m_gpuDoneCount++;
        NS_LOG_INFO("GPU " << gpu << " DONE (total done=" << m_gpuDoneCount << "/" << m_numGpus << ")");
        CheckGlobalCompletion();
        return;
    }

    // Schedule the next send for this GPU with optional ring-step overhead.
    uint64_t swOverhead = GetPerStepSwOverheadNs(m_chunkSize);
    if (swOverhead > 0)
    {
        Simulator::Schedule(NanoSeconds(swOverhead),
                            &RingAllReduce::SendGpuStepData, this, gpu, step + 1);
    }
    else
    {
        Simulator::ScheduleNow(&RingAllReduce::SendGpuStepData, this, gpu, step + 1);
    }
}

void
RingAllReduce::CheckGlobalCompletion()
{
    if (m_gpuDoneCount >= m_numGpus)
    {
        Complete();
    }
}

CollectiveType
RingAllReduce::GetCollectiveType() const
{
    return CollectiveType::ALLREDUCE;
}

bool
RingAllReduce::IsCompleted() const
{
    return m_state == RingState::COMPLETED;
}

void
RingAllReduce::Complete()
{
    m_state = RingState::COMPLETED;
    m_endTimeNs = Simulator::Now().GetNanoSeconds();

    uint64_t durationNs = m_endTimeNs - m_startTimeNs;

    NS_LOG_INFO("Pipelined Ring AllReduce COMPLETE in " << durationNs / 1000.0 << " µs");

    if (m_completionCallback)
    {
        m_completionCallback(durationNs);
    }

}

} // namespace ns3
