/*
 * SPDX-License-Identifier: GPL-2.0-only
 * Ethernet (RoCE v2) switch for the scale-out fabric.
 */

#include "ethernet-switch.h"

#include "fabric-header.h"
#include "roce-protocol-model.h" // RoceProtocol::SIMPLE
#include "so-header.h"
#include "ns3/log.h"
#include "ns3/packet.h"
#include "ns3/uinteger.h"

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
                            .AddConstructor<EthernetSwitch>()
                            .AddAttribute("EcnThreshold",
                                          "VOQ depth (packets) at which the switch "
                                          "marks the SoHeader ECN bit on enqueue",
                                          UintegerValue(8),
                                          MakeUintegerAccessor(&EthernetSwitch::m_ecnThreshold),
                                          MakeUintegerChecker<uint32_t>(0));
    return tid;
}

EthernetSwitch::EthernetSwitch()
    : m_ecnThreshold(8)
{
}

std::string
EthernetSwitch::GetVendorName() const
{
    return "Ethernet-RoCEv2";
}

void
EthernetSwitch::OnPacketEnqueue(Ptr<Packet> packet, uint32_t outPort,
                                size_t voqDepth)
{
    (void)outPort;
    MarkEcnIfCongested(packet, voqDepth);
}

bool
EthernetSwitch::MarkEcnIfCongested(Ptr<Packet> packet, size_t voqDepth) const
{
    if (!packet || voqDepth < m_ecnThreshold)
    {
        return false;
    }

    // Only RoCE So data packets carry an inner SoHeader to mark; control
    // packets (CREDIT/ACK/NACK/...) and non-RoCE packets are skipped -- they
    // have no SoHeader, and control packets must bypass congestion marking
    // just as they bypass BER (else credit/CNP feedback would deadlock).
    FabricHeader fh;
    packet->PeekHeader(fh);
    if (fh.IsControlPacket())
    {
        return false;
    }
    if (fh.GetProtocol() != static_cast<uint8_t>(RoceProtocol::SIMPLE))
    {
        return false;
    }

    // FabricHeader is outermost here (re-added by the datapath on ingress);
    // peel it, mark the inner SoHeader ECN bit, restore the layering.
    packet->RemoveHeader(fh);
    SoHeader soh;
    packet->RemoveHeader(soh);
    soh.SetEcn(true);
    packet->AddHeader(soh);
    packet->AddHeader(fh);
    return true;
}

} // namespace ns3
