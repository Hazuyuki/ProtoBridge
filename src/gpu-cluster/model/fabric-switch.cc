/*
 * SPDX-License-Identifier: GPL-2.0-only
 * fabric-switch.cc
 */

#include "fabric-switch.h"
#include "ns3/log.h"

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("FabricSwitch");

NS_OBJECT_ENSURE_REGISTERED(FabricSwitch);

TypeId
FabricSwitch::GetTypeId()
{
    static TypeId tid = TypeId("ns3::FabricSwitch")
                            .SetParent<NetDevice>()
                            .SetGroupName("GpuCluster");
    return tid;
}

FabricSwitch::FabricSwitch()
{
}

FabricSwitch::~FabricSwitch()
{
}

} // namespace ns3