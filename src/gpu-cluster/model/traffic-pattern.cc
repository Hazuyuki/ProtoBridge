/*
 * SPDX-License-Identifier: GPL-2.0-only
 * Author: GPU Cluster Simulator Team
 */

#include "traffic-pattern.h"
#include "fabric-endpoint.h"

#include "ns3/log.h"
#include "ns3/simulator.h"

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("TrafficPattern");

NS_OBJECT_ENSURE_REGISTERED(TrafficPattern);

TypeId
TrafficPattern::GetTypeId()
{
    static TypeId tid = TypeId("ns3::TrafficPattern")
                            .SetParent<Object>()
                            .SetGroupName("GpuCluster");
    return tid;
}

TrafficPattern::TrafficPattern()
    : m_running(false)
{
}

TrafficPattern::~TrafficPattern()
{
}

void
TrafficPattern::DoDispose()
{
    Stop();
    m_endpoint = nullptr;
    Object::DoDispose();
}

void
TrafficPattern::SetEndpoint(Ptr<FabricEndpoint> endpoint)
{
    m_endpoint = endpoint;
}

Ptr<FabricEndpoint>
TrafficPattern::GetEndpoint() const
{
    return m_endpoint;
}

void
TrafficPattern::Start()
{
    if (m_running)
    {
        return;
    }
    m_running = true;
    DoStart();
}

void
TrafficPattern::Stop()
{
    if (!m_running)
    {
        return;
    }
    m_running = false;
    DoStop();
}

bool
TrafficPattern::IsRunning() const
{
    return m_running;
}

void
TrafficPattern::SetCompleteCallback(CompleteCallback cb)
{
    m_completeCallback = cb;
}

void
TrafficPattern::NotifyComplete()
{
    if (!m_running)
    {
        return;
    }
    m_running = false;
    DoStop();
    if (!m_completeCallback.IsNull())
    {
        m_completeCallback();
    }
}

} // namespace ns3
