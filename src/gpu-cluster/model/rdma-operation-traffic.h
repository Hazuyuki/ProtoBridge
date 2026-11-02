/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef RDMA_OPERATION_TRAFFIC_H
#define RDMA_OPERATION_TRAFFIC_H
#include "fabric-endpoint.h"

namespace ns3
{
/** Small native workload for exercising verbs without a GPU simulator. */
class RdmaOperationTraffic : public Object
{
  public:
    static TypeId GetTypeId();
    void Initialize(Ptr<FabricEndpoint> source,
                    Ptr<FabricEndpoint> peer,
                    uint64_t bytes,
                    uint32_t iterations,
                    const RdmaConfig& config);
    void SetCompletionCallback(std::function<void(bool, uint64_t)> callback);
    void Start();
    uint64_t GetCompleted() const;
    uint64_t GetFailed() const;
    uint64_t GetLastValue() const;
    uint64_t GetSignalValue() const;
    uint64_t GetRemoteValue() const;

  protected:
    void DoDispose() override;

  private:
    void PostNextReceive();
    void TryComplete();
    Ptr<FabricEndpoint> m_source, m_peer;
    RdmaConfig m_config;
    RdmaOpcode m_opcode{RdmaOpcode::WRITE};
    uint64_t m_bytes{0}, m_completed{0}, m_failed{0}, m_lastValue{0};
    uint32_t m_iterations{0};
    uint64_t m_expectedReceives{0}, m_posted{0}, m_received{0}, m_startNs{0};
    bool m_signalReady{true}, m_done{false};
    std::function<void(bool, uint64_t)> m_complete;
    EventId m_postEvent;
};
} // namespace ns3
#endif
