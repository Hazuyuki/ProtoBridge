/*
 * SPDX-License-Identifier: GPL-2.0-only
 * Author: GPU Cluster Simulator Team
 *
 * Virtual Output Queue entry. Extracted to its own header so the arbiter
 * strategy interface (arbiter.h) can reference std::queue<VoqEntry> without
 * pulling in the full NvSwitch definition.
 */

#ifndef VOQ_ENTRY_H
#define VOQ_ENTRY_H

#include "ns3/mac48-address.h"
#include "ns3/packet.h"

#include <cstdint>

namespace ns3
{

/**
 * @ingroup gpu-cluster
 * @brief Virtual Output Queue entry
 */
struct VoqEntry
{
    Ptr<Packet> packet;     ///< The packet
    Mac48Address srcAddr;   ///< Source MAC address
    Mac48Address dstAddr;   ///< Destination MAC address
    uint32_t inPort;        ///< Input port number
};

} // namespace ns3

#endif // VOQ_ENTRY_H
