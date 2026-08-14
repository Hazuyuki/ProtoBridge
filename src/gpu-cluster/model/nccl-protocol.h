/*
 * nccl-protocol.h
 *
 * NVIDIA NCCL protocol enumeration.
 * NCCL uses different protocols based on message size:
 * - LL: Low Latency, 50% payload efficiency (64 data + 64 flags per 128B line)
 * - LL128: 93.75% payload efficiency (8 header + 120 data per 128B chunk)
 * - SIMPLE: 100% payload efficiency (pure data, no per-chunk overhead)
 */

#ifndef NCCL_PROTOCOL_H
#define NCCL_PROTOCOL_H

#include <cstdint>

namespace ns3
{

enum class NcclProtocol : uint8_t
{
    NONE = 0,
    LL = 1,
    LL128 = 2,
    SIMPLE = 3
};

inline const char*
NcclProtocolToString(NcclProtocol protocol)
{
    switch (protocol)
    {
        case NcclProtocol::NONE:   return "NONE";
        case NcclProtocol::LL:     return "LL";
        case NcclProtocol::LL128:  return "LL128";
        case NcclProtocol::SIMPLE: return "SIMPLE";
        default:                   return "UNKNOWN";
    }
}

} // namespace ns3

#endif // NCCL_PROTOCOL_H