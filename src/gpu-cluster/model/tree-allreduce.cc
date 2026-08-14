/*
 * tree-allreduce.cc
 *
 * Pipelined Binary Tree AllReduce: bottom-up reduce + top-down broadcast.
 *
 * For N=8 GPUs with depth=3:
 * - Total steps = 2*depth = 6 (3 reduce + 3 broadcast)
 * - Reduce step k (0..depth-1): GPUs at level (depth-1-k) send chunkSize to parent
 *   Step 0: leaves send, Step 1: mid-level sends, Step 2: root's children send
 * - Broadcast step k (depth..2*depth-1): GPUs at level (k-depth) send chunkSize to children
 *   Step 3: root sends, Step 4: mid-level sends, Step 5: leaves' parents send
 * - GPUs skip steps they don't participate in, advancing immediately
 * - Pipeline overlap: different GPUs can be on different steps
 */

#include "tree-allreduce.h"
#include "ns3/log.h"
#include "ns3/simulator.h"
#include "nccl-protocol.h"
#include "protocol-model.h"

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("TreeAllReduce");

NS_OBJECT_ENSURE_REGISTERED(TreeAllReduce);

TypeId
TreeAllReduce::GetTypeId()
{
    static TypeId tid = TypeId("ns3::TreeAllReduce")
                            .SetParent<CollectiveInjector>()
                            .SetGroupName("GpuCluster")
                            .AddConstructor<TreeAllReduce>();
    return tid;
}

TreeAllReduce::TreeAllReduce()
    : m_numGpus(0),
      m_dataSize(0),
      m_chunkSize(0),
      m_flowId(1),
      m_protocolId(0),
      m_depth(0),
      m_totalSteps(0),
      m_state(TreeAllReduceState::IDLE),
      m_startupDelayNs(0),
      m_perStepSwOverheadNs(0),
      m_swOverheadPerByteNs(0),
      m_gpuDoneCount(0),
      m_startTimeNs(0),
      m_endTimeNs(0)
{
}

TreeAllReduce::~TreeAllReduce()
{
}

void
TreeAllReduce::DoDispose()
{
    m_endpoints.clear();
    m_completionCallback = nullptr;
    CollectiveInjector::DoDispose();
}

uint16_t
TreeAllReduce::GetParent(uint16_t gpu) const
{
    if (gpu == 0xFFFF)
    {
        return 0xFFFF;
    }
    // Heap-arithmetic binary tree (correct, balanced depth=log2(N)).
    // NOTE: the G3.1 topology-embedded parent (m_endpoints[gpu]->GetTreeParent())
    // is not used here because it produces a degenerate tree on several
    // topologies (e.g. switched: depth=5 for N=4, zero matched senders),
    // which makes the collective complete at startup with no wire time.
    if (gpu == 0) return 0xFFFF;
    return (gpu - 1) / 2;
}

uint16_t
TreeAllReduce::GetLeftChild(uint16_t gpu) const
{
    uint16_t child = 2 * gpu + 1;
    return (child < m_numGpus) ? child : 0xFFFF;
}

uint16_t
TreeAllReduce::GetRightChild(uint16_t gpu) const
{
    uint16_t child = 2 * gpu + 2;
    return (child < m_numGpus) ? child : 0xFFFF;
}

uint16_t
TreeAllReduce::GetTreeDepth() const
{
    // Heap-arithmetic balanced binary tree: depth = ceil(log2(N)).
    // (round up so non-power-of-2 N still reaches every GPU.)
    uint16_t depth = 0;
    uint32_t n = m_numGpus;
    while (n > 1)
    {
        n = (n + 1) / 2;
        depth++;
    }
    return depth ? depth : 1;
}

uint16_t
TreeAllReduce::GetTreeLevel(uint16_t gpu) const
{
    // Walk parents (topology-aware when embedded) to the root. Guards against
    // cycles by capping the walk at m_numGpus levels.
    uint16_t level = 0;
    uint16_t g = gpu;
    uint16_t guard = 0;
    while (g != 0xFFFF && GetParent(g) != 0xFFFF && guard < m_numGpus + 1)
    {
        g = GetParent(g);
        level++;
        guard++;
    }
    return level;
}

bool
TreeAllReduce::IsReduceStep(uint32_t step) const
{
    return step < m_depth;
}

bool
TreeAllReduce::IsBroadcastStep(uint32_t step) const
{
    return step >= m_depth && step < m_totalSteps;
}

void
TreeAllReduce::Initialize(uint16_t numGpus, uint64_t dataSize,
                           const std::vector<Ptr<FabricEndpoint>>& endpoints)
{
    m_numGpus = numGpus;
    m_dataSize = dataSize;
    m_endpoints = endpoints;

    m_depth = GetTreeDepth();
    m_totalSteps = 2 * m_depth;

    // Chunk size: same as ring segment size (dataSize/numGpus) for consistent
    // pipeline behavior. Each step processes one chunk through one tree hop.
    m_chunkSize = dataSize / numGpus;
    if (m_chunkSize == 0) m_chunkSize = dataSize;

    m_gpuCurrentStep.resize(numGpus, 0);
    m_gpuStepReceivedBytes.assign(m_totalSteps, std::vector<uint64_t>(numGpus, 0));
    m_gpuDoneCount = 0;

    m_flowId = 1;

    // Protocol selection based on chunk size (per-step payload)
    Ptr<ProtocolModel> protoModel = m_endpoints[0]->GetProtocolModel();
    if (m_startupDelayNs == 0)
    {
        uint8_t protoId = protoModel->GetProtocolId(dataSize);
        m_startupDelayNs = protoModel->GetStartupDelayNs(protoId, m_numGpus);
    }
    NcclProtocol wireProto = static_cast<NcclProtocol>(protoModel->GetProtocolId(m_chunkSize));
    m_protocolId = static_cast<uint8_t>(wireProto);

    // Pre-compute per-step send targets and receive thresholds
    m_stepSendTarget.resize(m_totalSteps, std::vector<uint16_t>(numGpus, 0xFFFF));
    m_stepReceiveThreshold.resize(m_totalSteps, std::vector<uint64_t>(numGpus, 0));

    for (uint32_t step = 0; step < m_totalSteps; step++)
    {
        for (uint16_t gpu = 0; gpu < numGpus; gpu++)
        {
            uint16_t level = GetTreeLevel(gpu);

            if (IsReduceStep(step))
            {
                // Reduce step k: GPUs at level (depth-1-k) send chunkSize to parent
                uint32_t k = step;
                uint16_t targetLevel = m_depth - 1 - k;

                if (level == targetLevel && GetParent(gpu) != 0xFFFF)
                {
                    m_stepSendTarget[step][gpu] = GetParent(gpu);
                    // Parent accumulates: add chunkSize to parent's receive threshold
                    m_stepReceiveThreshold[step][GetParent(gpu)] += m_chunkSize;
                }
            }
            else
            {
                // Broadcast step k: GPUs at level (k-depth) send chunkSize to children
                uint32_t k = step - m_depth;
                uint16_t senderLevel = k;

                if (level == senderLevel)
                {
                    uint16_t left = GetLeftChild(gpu);
                    uint16_t right = GetRightChild(gpu);

                    if (left != 0xFFFF)
                    {
                        m_stepSendTarget[step][gpu] = left;
                        m_stepReceiveThreshold[step][left] += m_chunkSize;
                    }
                    if (right != 0xFFFF)
                    {
                        m_stepReceiveThreshold[step][right] += m_chunkSize;
                    }
                }
            }
        }
    }

    // Root GPU starts at the step where it first receives data (not at broadcast phase).
    // Root receives from its children in reduce step (depth-1), so start there.
    // If root has no receive steps (N=2, depth=1), start at broadcast step (depth).
    uint64_t rootFirstRecv = 0;
    for (uint32_t s = 0; s < m_totalSteps; s++)
    {
        if (m_stepReceiveThreshold[s][0] > 0)
        {
            rootFirstRecv = s;
            break;
        }
    }
    if (rootFirstRecv > 0 || m_stepReceiveThreshold[0][0] > 0)
    {
        m_gpuCurrentStep[0] = rootFirstRecv;
    }
    else
    {
        m_gpuCurrentStep[0] = m_depth;  // Pure sender root (e.g., N=2)
    }

    SetupReceiveCallbacks();

    NS_LOG_INFO("Pipelined TreeAllReduce initialized: numGpus=" << numGpus
                << " dataSize=" << dataSize << " chunkSize=" << m_chunkSize
                << " depth=" << m_depth << " totalSteps=" << m_totalSteps
                << " startupDelay=" << m_startupDelayNs << "ns"
                << " protocol=" << static_cast<int>(wireProto));
}

void
TreeAllReduce::SetupReceiveCallbacks()
{
    for (auto& ep : m_endpoints)
    {
        if (ep)
        {
            ep->SetReceiveCallback(
                MakeCallback(&TreeAllReduce::OnPacketReceived, this));
        }
    }
}

void
TreeAllReduce::SetCompletionCallback(std::function<void(uint64_t durationNs)> cb)
{
    m_completionCallback = cb;
}

void
TreeAllReduce::SetStartupDelay(Time delay)
{
    m_startupDelayNs = delay.GetNanoSeconds();
}

void
TreeAllReduce::SetPerStepSwOverhead(Time delay)
{
    m_perStepSwOverheadNs = delay.GetNanoSeconds();
}

void
TreeAllReduce::SetSwOverheadPerByteNs(uint64_t nsPerByte)
{
    m_swOverheadPerByteNs = nsPerByte;
}

void
TreeAllReduce::Start()
{
    m_state = TreeAllReduceState::RUNNING;
    m_startTimeNs = Simulator::Now().GetNanoSeconds();

    NS_LOG_INFO("Starting Pipelined Tree AllReduce with startup delay " << m_startupDelayNs << " ns");

    Simulator::Schedule(NanoSeconds(m_startupDelayNs), &TreeAllReduce::TriggerInitialSends, this);
}

void
TreeAllReduce::TriggerInitialSends()
{
    // All GPUs start advancing from their initial step
    for (uint16_t gpu = 0; gpu < m_numGpus; gpu++)
    {
        AdvanceGpuStep(gpu);
    }
}

void
TreeAllReduce::AdvanceGpuStep(uint16_t gpu)
{
    uint32_t step = m_gpuCurrentStep[gpu];

    while (step < m_totalSteps)
    {
        uint16_t sendTarget = m_stepSendTarget[step][gpu];
        uint64_t recvThreshold = m_stepReceiveThreshold[step][gpu];

        if (sendTarget != 0xFFFF)
        {
            // This GPU needs to send in this step
            SendGpuStepData(gpu, step);

            // For broadcast steps where GPU has both send AND receive targets
            // (e.g., internal nodes that receive from parent AND send to children):
            // they need to wait for receive before sending, but only if there's
            // a receive threshold for this GPU in this same step.
            // In our pre-computed tables, a GPU that sends in broadcast step
            // also receives from its parent in that step (for internal nodes).
            // However, for the FIRST broadcast step (root sends), root only sends.
            // For reduce steps, sending GPUs don't also receive in the same step
            // (they only receive in a previous step).

            // After sending, check if this GPU also needs to receive
            if (recvThreshold > 0)
            {
                // Wait for receive data before advancing
                return;
            }

            // Pure sender (no receive in this step): advance immediately
            m_gpuCurrentStep[gpu] = step + 1;
            step = step + 1;
            continue;
        }
        else if (recvThreshold > 0)
        {
            // Pure receiver: wait for data
            return;
        }
        else
        {
            // Neither sender nor receiver: skip this step
            m_gpuCurrentStep[gpu] = step + 1;
            step = step + 1;
            continue;
        }
    }

    // GPU has completed all steps
    m_gpuDoneCount++;
    NS_LOG_INFO("GPU " << gpu << " DONE (total done=" << m_gpuDoneCount << "/" << m_numGpus << ")");
    CheckGlobalCompletion();
}

void
TreeAllReduce::SendGpuStepData(uint16_t gpu, uint32_t step)
{
    if (gpu >= m_endpoints.size() || !m_endpoints[gpu])
    {
        return;
    }

    uint16_t target = m_stepSendTarget[step][gpu];
    if (target == 0xFFFF)
    {
        return;
    }

    std::vector<uint8_t> data(m_chunkSize, 0xAB);
    uint16_t stepFlowId = m_flowId + step;

    m_endpoints[gpu]->SendBulkWireTransfer(target, data.data(), m_chunkSize, m_protocolId, stepFlowId, 0);

    NS_LOG_DEBUG("GPU " << gpu << " step " << step
                 << " sending " << m_chunkSize << " bytes to GPU " << target
                 << " (" << (IsReduceStep(step) ? "REDUCE" : "BROADCAST")
                 << " flowId=" << stepFlowId << ")");

    // For broadcast steps with two children, also send to right child
    if (IsBroadcastStep(step))
    {
        uint16_t right = GetRightChild(gpu);
        if (right != 0xFFFF)
        {
            std::vector<uint8_t> data2(m_chunkSize, 0xAB);
            uint16_t stepFlowId2 = m_flowId + step + 100;  // different flowId for second child
            m_endpoints[gpu]->SendBulkWireTransfer(right, data2.data(), m_chunkSize, m_protocolId, stepFlowId2, 0);
            NS_LOG_DEBUG("GPU " << gpu << " step " << step
                         << " also sending to right child " << right);
        }
    }
}

void
TreeAllReduce::OnPacketReceived(uint16_t srcRank, Ptr<Packet> packet, FabricHeader header)
{
    if (header.GetPacketType() != FabricPacketType::DATA)
    {
        return;
    }

    uint16_t destRank = header.GetDestRank();
    if (destRank >= m_numGpus)
    {
        return;
    }

    // Skip packets for GPUs that have already completed all steps
    if (m_gpuCurrentStep[destRank] >= m_totalSteps)
    {
        return;
    }

    uint32_t effectiveSize = header.GetEffectiveDataSize();
    if (effectiveSize == 0)
    {
        effectiveSize = packet->GetSize();
    }

    uint32_t packetStep = GetStepFromFlowId(header.GetFlowId());
    if (packetStep >= m_totalSteps)
    {
        return;
    }

    uint32_t currentStep = m_gpuCurrentStep[destRank];
    m_gpuStepReceivedBytes[packetStep][destRank] += effectiveSize;

    NS_LOG_DEBUG("Tree packet: src=" << srcRank << " -> dest=" << destRank
                 << " effectiveSize=" << effectiveSize
                 << " totalReceived=" << m_gpuStepReceivedBytes[packetStep][destRank]
                 << " packetStep=" << packetStep
                 << " currentStep=" << currentStep
                 << " threshold=" << m_stepReceiveThreshold[packetStep][destRank]);

    uint64_t threshold = m_stepReceiveThreshold[packetStep][destRank];
    if (packetStep == currentStep && threshold > 0
        && m_gpuStepReceivedBytes[packetStep][destRank] >= threshold)
    {
        m_gpuStepReceivedBytes[packetStep][destRank] = 0;
        m_gpuCurrentStep[destRank] = currentStep + 1;

        NS_LOG_INFO("GPU " << destRank << " completed step " << currentStep
                     << " (now at step " << m_gpuCurrentStep[destRank] << ")");

        OnGpuStepReceiveComplete(destRank, currentStep);
    }
}

uint32_t
TreeAllReduce::GetStepFromFlowId(uint16_t flowId) const
{
    uint32_t offset = static_cast<uint16_t>(flowId - m_flowId);
    if (offset >= 100 && offset < 100 + m_totalSteps)
    {
        offset -= 100;
    }
    return offset;
}

void
TreeAllReduce::OnGpuStepReceiveComplete(uint16_t gpu, uint32_t step)
{
    LogStepComplete(gpu, step, m_startTimeNs, Simulator::Now().GetNanoSeconds());

    // GPU has received data for this step — apply per-step software overhead
    // (models NCCL kernel scheduling, barrier sync, and memory staging;
    // NOT GPU reduction compute, which is negligible at teraflops rates)
    // Use the receive threshold for this step (actual bytes consumed),
    // not chunkSize, since parent nodes receive from multiple children.
    uint64_t bytesProcessed = m_stepReceiveThreshold[step][gpu];
    if (bytesProcessed == 0) bytesProcessed = m_chunkSize;
    uint64_t swOverhead = m_perStepSwOverheadNs + m_swOverheadPerByteNs * bytesProcessed;
    if (swOverhead > 0)
    {
        Simulator::Schedule(NanoSeconds(swOverhead),
                            &TreeAllReduce::AdvanceGpuStep, this, gpu);
    }
    else
    {
        AdvanceGpuStep(gpu);
    }
}

void
TreeAllReduce::CheckGlobalCompletion()
{
    if (m_gpuDoneCount >= m_numGpus)
    {
        Complete();
    }
}

CollectiveType
TreeAllReduce::GetCollectiveType() const
{
    return CollectiveType::ALLREDUCE;
}

bool
TreeAllReduce::IsCompleted() const
{
    return m_state == TreeAllReduceState::COMPLETED;
}

void
TreeAllReduce::Complete()
{
    m_state = TreeAllReduceState::COMPLETED;
    m_endTimeNs = Simulator::Now().GetNanoSeconds();

    uint64_t durationNs = m_endTimeNs - m_startTimeNs;

    NS_LOG_INFO("Pipelined Tree AllReduce COMPLETE in " << durationNs / 1000.0 << " µs");

    if (m_completionCallback)
    {
        m_completionCallback(durationNs);
    }

}

} // namespace ns3
