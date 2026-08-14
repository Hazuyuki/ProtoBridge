/*
 * SPDX-License-Identifier: GPL-2.0-only
 * Author: GPU Cluster Simulator Team
 *
 * Traffic Pattern Abstraction for generating different traffic types
 */

#ifndef TRAFFIC_PATTERN_H
#define TRAFFIC_PATTERN_H

#include "ns3/object.h"
#include "ns3/ptr.h"
#include "ns3/callback.h"
#include "ns3/nstime.h"

namespace ns3
{

class FabricEndpoint;

/**
 * @ingroup gpu-cluster
 * @brief Abstract base class for traffic patterns
 *
 * Provides a common interface for all traffic generators:
 * P2P, memory access, and collective traffic.
 */
class TrafficPattern : public Object
{
  public:
    static TypeId GetTypeId();

    TrafficPattern();
    ~TrafficPattern() override;

    TrafficPattern(const TrafficPattern&) = delete;
    TrafficPattern& operator=(const TrafficPattern&) = delete;

    void SetEndpoint(Ptr<FabricEndpoint> endpoint);
    Ptr<FabricEndpoint> GetEndpoint() const;

    void Start();
    void Stop();
    bool IsRunning() const;

    typedef Callback<void> CompleteCallback;
    void SetCompleteCallback(CompleteCallback cb);

  protected:
    virtual void DoStart() = 0;
    virtual void DoStop() = 0;

    void NotifyComplete();

    Ptr<FabricEndpoint> m_endpoint;
    bool m_running;

  private:
    void DoDispose() override;

    CompleteCallback m_completeCallback;
};

} // namespace ns3

#endif /* TRAFFIC_PATTERN_H */
