/*
 * SPDX-License-Identifier: GPL-2.0-only
 * protocol-payload-builder.cc
 */

#include "protocol-payload-builder.h"
#include "ns3/log.h"

namespace ns3 {

NS_LOG_COMPONENT_DEFINE("ProtocolPayloadBuilder");

NS_OBJECT_ENSURE_REGISTERED(ProtocolPayloadBuilder);

TypeId
ProtocolPayloadBuilder::GetTypeId()
{
    static TypeId tid = TypeId("ns3::ProtocolPayloadBuilder")
                            .SetParent<Object>()
                            .SetGroupName("GpuCluster");
    return tid;
}

ProtocolPayloadBuilder::ProtocolPayloadBuilder()
{
}

ProtocolPayloadBuilder::~ProtocolPayloadBuilder()
{
}

} // namespace ns3