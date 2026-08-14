/*
 * flow-control-policy.h
 *
 * Flow-control policy seam. The endpoint's send gate dispatches on this
 * enum; CREDIT is fully implemented (per-VC CreditManager), WINDOW and
 * RATE are exposed as selectable options but not yet implemented (the
 * gate admits and logs). This lets a profile select a policy today and
 * lets a future implementation fill in WINDOW/RATE without touching the
 * send-gate call sites again.
 */

#ifndef FLOW_CONTROL_POLICY_H
#define FLOW_CONTROL_POLICY_H

#include <cstdint>
#include <string>

namespace ns3
{

enum class FlowControlPolicy : uint8_t
{
    CREDIT = 0, ///< Credit-based, per-VC (implemented: CreditManager)
    WINDOW = 1, ///< Window-based (seam only, not implemented)
    RATE = 2    ///< Rate-based (seam only, not implemented)
};

inline std::string
FlowControlPolicyName(FlowControlPolicy p)
{
    switch (p)
    {
    case FlowControlPolicy::CREDIT: return "credit";
    case FlowControlPolicy::WINDOW: return "window";
    case FlowControlPolicy::RATE: return "rate";
    }
    return "unknown";
}

inline FlowControlPolicy
FlowControlPolicyFromString(const std::string& s)
{
    if (s == "window") return FlowControlPolicy::WINDOW;
    if (s == "rate") return FlowControlPolicy::RATE;
    return FlowControlPolicy::CREDIT; // default + fallback for unknown
}

} // namespace ns3

#endif // FLOW_CONTROL_POLICY_H
