/*
 * SPDX-License-Identifier: GPL-2.0-only
 * Ethernet (RoCEv2) switch for the scale-out fabric.
 */

#include "ethernet-switch.h"

#include "ns3/log.h"

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("EthernetSwitch");
NS_OBJECT_ENSURE_REGISTERED(EthernetSwitch);

TypeId
EthernetSwitch::GetTypeId()
{
    static TypeId tid = TypeId("ns3::EthernetSwitch")
                            .SetParent<NvSwitch>()
                            .SetGroupName("GpuCluster")
                            .AddConstructor<EthernetSwitch>();
    return tid;
}

std::string
EthernetSwitch::GetVendorName() const
{
    return "Ethernet-RoCEv2";
}

} // namespace ns3
