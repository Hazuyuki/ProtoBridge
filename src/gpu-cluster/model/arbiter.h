/*
 * SPDX-License-Identifier: GPL-2.0-only
 * Author: GPU Cluster Simulator Team
 *
 * Arbiter — crossbar arbitration strategy interface.
 *
 * The NVSwitch datapath is split into two concerns:
 *   1. ARBITRATION (this interface): given the per-output-port VOQs and the
 *      per-port egress-busy-until times, decide which output ports are granted
 *      a departure this cycle.
 *   2. FORWARDING + RESCHEDULING (NvSwitch): drain the granted VOQ front
 *      packet onto the wire and schedule the next arbitration wake-up.
 *
 * The default strategy, RoundRobinArbiter, grants every output port whose VOQ
 * is non-empty and whose egress link is free — i.e. it reproduces the
 * non-blocking crossbar model the simulator has always used. Subclass Arbiter
 * to plug a different policy (weighted fair queuing, strict priority by VC,
 * iQoS, …) without touching NvSwitch internals; select it on a switch via
 * NvSwitch::SetArbiter() or the "Arbiter" TypeId attribute.
 */

#ifndef ARBITER_H
#define ARBITER_H

#include "ns3/object.h"
#include "ns3/nstime.h"
#include "voq-entry.h"

#include <queue>
#include <vector>
#include <cstdint>

namespace ns3
{

/**
 * @ingroup gpu-cluster
 * @brief A single arbitration grant: "drain one packet from VOQ[port] now".
 */
struct ArbiterGrant
{
    uint32_t port; ///< Output port index to drain
};

/**
 * @ingroup gpu-cluster
 * @brief Abstract crossbar arbitration strategy.
 *
 * Implementations inspect the current VOQ + egress-busy state at a wake-up and
 * return the set of grants the switch should action. The contract is one
 * packet per granted port per call (egress serialization); grants are acted on
 * in the order returned.
 */
class Arbiter : public Object
{
  public:
    static TypeId GetTypeId();
    Arbiter();
    ~Arbiter() override;

    /**
     * @brief Select which output ports to drain this arbitration wake-up.
     * @param voqs Per-output-port VOQs (indexed by output port). Read-only.
     * @param outputBusyUntil Per-port egress busy-until time (indexed by port).
     * @param now The current simulation time.
     * @return Grants to action, one packet per port (drain the VOQ front).
     */
    virtual std::vector<ArbiterGrant>
    SelectGrants(const std::vector<std::queue<VoqEntry>>& voqs,
                 const std::vector<Time>& outputBusyUntil,
                 Time now) = 0;

    /// Human-readable policy name (for logging / config echo).
    virtual std::string GetName() const = 0;
};

/**
 * @ingroup gpu-cluster
 * @brief Default arbiter: grant every port whose VOQ is non-empty and whose
 * egress link is free. Models a non-blocking crossbar with per-port egress
 * serialization — the simulator's historical behavior.
 */
class RoundRobinArbiter : public Arbiter
{
  public:
    static TypeId GetTypeId();
    RoundRobinArbiter();
    ~RoundRobinArbiter() override;

    std::vector<ArbiterGrant>
    SelectGrants(const std::vector<std::queue<VoqEntry>>& voqs,
                 const std::vector<Time>& outputBusyUntil,
                 Time now) override;

    std::string GetName() const override;
};

} // namespace ns3

#endif // ARBITER_H
