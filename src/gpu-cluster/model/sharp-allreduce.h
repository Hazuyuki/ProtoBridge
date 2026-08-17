/*
 * SPDX-License-Identifier: GPL-2.0-only
 * sharp-allreduce.h
 *
 * SHARP (Scalable Hierarchical Aggregation and Reduction Protocol) AllReduce.
 * Derives from CollectiveInjector.
 *
 * Algorithm: tree-based reduction through NVSwitch.
 * - Reduce phase: all GPUs send their data to the switch simultaneously
 * - Switch aggregation: switch buffers contributions from all GPUs, then
 *   multicasts reduced result to ALL GPUs
 * - Steps: 2 logical steps (send-to-switch, receive-from-switch)
 *   vs Ring's 2*(N-1) steps
 *
 * Requires "switched" topology with NVSwitch that has AllReduceEnabled=true.
 */

#ifndef SHARP_ALLREDUCE_H
#define SHARP_ALLREDUCE_H

#include "collective-injector.h"
#include "fabric-endpoint.h"
#include "fabric-header.h"
#include "protocol-transaction.h"

#include <vector>
#include <functional>

namespace ns3
{

enum class SharpState
{
    IDLE,
    REDUCE_PHASE,
    BROADCAST_PHASE,
    COMPLETED
};

class SharpAllReduce : public CollectiveInjector
{
  public:
    static TypeId GetTypeId();

    SharpAllReduce();
    virtual ~SharpAllReduce();

    // CollectiveInjector overrides
    void Initialize(uint16_t numGpus, uint64_t dataSize,
                    const std::vector<Ptr<FabricEndpoint>>& endpoints) override;
    void SetCompletionCallback(std::function<void(uint64_t durationNs)> cb) override;
    void Start() override;
    void SetStartupDelay(Time delay) override;
    CollectiveType GetCollectiveType() const override;
    bool IsCompleted() const override;

    SharpState GetState() const { return m_state; }

  private:
    void DoDispose() override;
    void SetupReceiveCallbacks();
    void OnPacketReceived(uint16_t srcRank, Ptr<Packet> packet, FabricHeader header);
    void BuildTransactionGraph();
    void ExecuteTransactionAction(const ProtocolTransactionAction& action);
    void OnTransactionComplete();
    void TriggerReducePhase();
    void OnGpuReduceComplete(uint16_t gpu);
    void OnGpuBroadcastReceiveComplete(uint16_t gpu);
    void Complete();

    uint16_t m_numGpus;
    uint64_t m_dataSize;
    uint16_t m_flowId;
    bool m_useTransactionModel;

    SharpState m_state;
    uint64_t m_startupDelayNs;

    // Track which GPUs have finished sending to switch (reduce phase)
    uint32_t m_reduceDoneCount;

    // Track received bytes per GPU for broadcast phase
    std::vector<uint64_t> m_gpuBroadcastReceivedBytes;
    uint32_t m_broadcastDoneCount;

    uint64_t m_startTimeNs;
    uint64_t m_endTimeNs;

    std::vector<Ptr<FabricEndpoint>> m_endpoints;
    ProtocolTransactionGraph m_transactionGraph;
    Ptr<ProtocolTransactionExecutor> m_transactionExecutor;
    std::function<void(uint64_t durationNs)> m_completionCallback;
};

} // namespace ns3

#endif // SHARP_ALLREDUCE_H
