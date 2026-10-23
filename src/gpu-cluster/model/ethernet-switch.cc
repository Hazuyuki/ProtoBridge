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
                                          MakeUintegerChecker<uint32_t>(0))
            .AddAttribute("VoqDropThreshold",
                          "VOQ depth (packets) at which a data packet is "
                          "tail-dropped with a NACK to the source (lossy "
                          "fabric). Must be < NvSwitch::VoqDepth, else "
                          "the base hard-cap drop preempts the NACK path. "
                          "Plain-Ethernet and RoCE packets alike: 0 = "
                          "lossless (RoCE falls through to ECN marking), "
                          "> 0 opts the fabric into the lossy/no-PFC "
                          "regime with LLR retransmit.",
                          UintegerValue(0),
                          MakeUintegerAccessor(&EthernetSwitch::m_voqDropThreshold),
                          MakeUintegerChecker<uint32_t>(0));
    return tid;
}

EthernetSwitch::EthernetSwitch()
    : m_ecnThreshold(8),
      m_voqDropThreshold(0)
{
}

std::string
EthernetSwitch::GetVendorName() const
{
    return "Ethernet-RoCEv2";
}

bool
EthernetSwitch::OnPacketEnqueue(Ptr<Packet> packet, uint32_t outPort,
                                uint32_t inPort, size_t voqDepth)
{
    (void)outPort;
    if (!packet)
    {
        return true;
    }
    FabricHeader fh;
    packet->PeekHeader(fh);

    // Lossy drop: a DATA packet (plain-Ethernet, or RoCE when
    // --voqDropThreshold > 0 opts into the lossy-RoCE/no-PFC regime) that
    // meets the VOQ drop threshold is tail-dropped with a NACK back to the
    // source, so the sender retransmits via the existing LLR/SACK path.
    // Control and retry packets bypass (matching the BER bypass) -- dropping
    // them would deadlock the credit/feedback loop. Default threshold 0
    // keeps BOTH fabrics lossless (RoCE falls through to ECN below).
    if (!fh.IsControlPacket() && !fh.IsRetryPacket()
        && (fh.GetFabricType() == FabricType::ETHERNET
            || (fh.GetFabricType() == FabricType::ROCE && m_voqDropThreshold > 0)))
    {
        // 0 = lossless (disable the lossy drop): the documented "0=off".
        // Without this guard `voqDepth >= 0` is always true and every
        // Ethernet DATA packet would be tail-dropped -- a total-stall
        // foot-gun (the opposite of the documented "off").
        if (m_voqDropThreshold == 0)
        {
            return true;
        }
        if (voqDepth >= m_voqDropThreshold)
        {
            EmitNackBack(fh, inPort, fh.GetSourceMac());
            return false; // drop: caller skips the push + counts m_voqDrops
        }
        if (fh.GetFabricType() == FabricType::ETHERNET)
        {
            return true; // admit; plain Ethernet carries no SoHeader to ECN-mark
        }
        // Lossy RoCE: fall through to ECN marking (ecnThreshold < drop
        // threshold gives mark-before-drop, the real RoCEv2 ordering).
    }

    // RoCE lossless path: mark the inner SoHeader ECN bit when congested.
    if (MarkEcnIfCongested(packet, voqDepth))
    {
        m_ecnMarkedPackets++;
    }
    return true;
}

bool
EthernetSwitch::MarkEcnIfCongested(Ptr<Packet> packet, size_t voqDepth) const
{
    // 0 = off (disable ECN marking): the CLI's documented contract
    // (gpu-cluster-sim "0=off; Phase-B reaction pending"). Without this
    // guard `voqDepth < 0` is never true, so even a quiescent VOQ (depth
    // 0) would satisfy `0 < 0 == false` and mark every RoCE data packet
    // -- one ECN mark + CNP on an otherwise idle link (codex round 24,
    // finding 7).
    if (!packet || m_ecnThreshold == 0 || voqDepth < m_ecnThreshold)
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
    // Only RoCE-fabric (So) DATA packets carry an inner SoHeader to mark.
    // RoceProtocol::SIMPLE(1) collides with NcclProtocol::LL(1); without the
    // fabric-type gate, a Su LL packet (protocol=1, no SoHeader) traversing
    // this switch would pass the protocol guard and RemoveHeader(soh) below
    // would read 10 B of NCCL LL payload as a SoHeader -- corrupting it.
    if (fh.GetProtocol() != static_cast<uint8_t>(RoceProtocol::SIMPLE) ||
        fh.GetFabricType() != FabricType::ROCE)
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
