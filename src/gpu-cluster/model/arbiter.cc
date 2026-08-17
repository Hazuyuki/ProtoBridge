/*
 * SPDX-License-Identifier: GPL-2.0-only
 * Author: GPU Cluster Simulator Team
 *
 * Arbiter strategy implementation.
 */

#include "arbiter.h"
#include "nvswitch.h" // VoqEntry definition

#include "ns3/log.h"

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("Arbiter");

NS_OBJECT_ENSURE_REGISTERED(Arbiter);

TypeId
Arbiter::GetTypeId()
{
    static TypeId tid = TypeId("ns3::Arbiter")
        .SetParent<Object>()
        .SetGroupName("GpuCluster");
    return tid;
}

Arbiter::Arbiter()
{
    NS_LOG_FUNCTION(this);
}

Arbiter::~Arbiter()
{
    NS_LOG_FUNCTION(this);
}

NS_OBJECT_ENSURE_REGISTERED(RoundRobinArbiter);

TypeId
RoundRobinArbiter::GetTypeId()
{
    static TypeId tid = TypeId("ns3::RoundRobinArbiter")
        .SetParent<Arbiter>()
        .SetGroupName("GpuCluster")
        .AddConstructor<RoundRobinArbiter>();
    return tid;
}

RoundRobinArbiter::RoundRobinArbiter()
{
    NS_LOG_FUNCTION(this);
}

RoundRobinArbiter::~RoundRobinArbiter()
{
    NS_LOG_FUNCTION(this);
}

std::vector<ArbiterGrant>
RoundRobinArbiter::SelectGrants(const std::vector<std::queue<VoqEntry>>& voqs,
                                const std::vector<Time>& outputBusyUntil,
                                Time now)
{
    // Per-output-port matching: grant every port whose VOQ is non-empty and
    // whose egress serialization is free. This is the non-blocking crossbar
    // model (multiple input-output pairs in parallel; one packet per output
    // port at a time).
    std::vector<ArbiterGrant> grants;
    uint32_t n = static_cast<uint32_t>(voqs.size());
    for (uint32_t port = 0; port < n; ++port)
    {
        if (!voqs[port].empty() && now >= outputBusyUntil[port])
        {
            grants.push_back({port});
        }
    }
    return grants;
}

std::string
RoundRobinArbiter::GetName() const
{
    return "roundrobin";
}

} // namespace ns3
