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
#include "ns3/simulator.h"
#include "ns3/uinteger.h"

#include <algorithm>

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
                          MakeUintegerChecker<uint32_t>(0))
            .AddAttribute("PfcMode",
                          "PFC (802.1Qbb-style) priority flow control: "
                          "0 = off (default; byte-identical legacy "
                          "behavior), 1 = static threshold (pause when a "
                          "VOQ crosses PfcPauseThreshold), 2 = dynamic "
                          "(HPCC SwitchMmu-style: the threshold follows "
                          "the remaining VOQ headroom, so a nearly-full "
                          "switch pauses earlier). With PFC on, keep "
                          "PfcPauseThreshold < VoqDropThreshold so the "
                          "pause fires before the tail-drop.",
                          UintegerValue(0),
                          MakeUintegerAccessor(&EthernetSwitch::m_pfcMode),
                          MakeUintegerChecker<uint32_t>(0, 2))
            .AddAttribute("PfcPauseThreshold",
                          "PFC: VOQ depth (packets) at which the switch "
                          "PAUSEs the ingress feeding the congested "
                          "egress (mode 1; the baseline for mode 2). "
                          "Inert when PfcMode=0.",
                          UintegerValue(64),
                          MakeUintegerAccessor(&EthernetSwitch::m_pfcPauseThreshold),
                          MakeUintegerChecker<uint32_t>(1))
            .AddAttribute("PfcResumeOffset",
                          "PFC resume hysteresis (packets): the pause "
                          "lifts once the VOQ drains to "
                          "PfcPauseThreshold - PfcResumeOffset. Guards "
                          "against pause/resume thrash around the "
                          "threshold. Inert when PfcMode=0.",
                          UintegerValue(8),
                          MakeUintegerAccessor(&EthernetSwitch::m_pfcResumeOffset),
                          MakeUintegerChecker<uint32_t>(0));
    return tid;
}

EthernetSwitch::EthernetSwitch()
    : m_ecnThreshold(8),
      m_voqDropThreshold(0),
      m_pfcMode(0),
      m_pfcPauseThreshold(64),
      m_pfcResumeOffset(8)
{
}

std::string
EthernetSwitch::GetVendorName() const
{
    return "Ethernet-RoCEv2";
}

bool
EthernetSwitch::IsPfcPaused(uint32_t inPort, uint32_t outPort) const
{
    auto it = m_pfcPaused.find({inPort, outPort});
    return it != m_pfcPaused.end() && it->second;
}

void
EthernetSwitch::PrunePfcResumeEvents()
{
    // Drop fired watchers (IsExpired) — the current poll rescheduled itself
    // before the ledger was pruned, so the vector's live tail is all that
    // needs to survive. Keeps the ledger O(live watchers) instead of
    // O(cumulative polls) (codex round 27, finding 9).
    std::erase_if(m_pfcResumeEvents,
                  [](const EventId& ev) { return ev.IsExpired(); });
}

void
EthernetSwitch::DoDispose()
{
    // Cancel every pending resume-hysteresis watcher: each targets raw `this`
    // and dereferences m_voqs/m_ports, which the base DoDispose clears below —
    // a survivor firing after disposal is the same UAF class the deferred-result
    // ledger fixed (codex round 15, finding 2).
    for (const EventId& ev : m_pfcResumeEvents)
    {
        if (!ev.IsExpired())
        {
            Simulator::Cancel(ev);
        }
    }
    m_pfcResumeEvents.clear();
    m_pfcPaused.clear();
    m_lastPfcPauseHeader.clear();
    NvSwitch::DoDispose();
}

// PFC trigger, evaluated per arriving packet. The threshold in mode 1 is the
// static PfcPauseThreshold; in mode 2 it is dynamic — the HPCC SwitchMmu
// contract (GetPfcThreshold: (buffer - used) >> shift) compressed to our
// single-priority VOQ model: threshold = PfcPauseThreshold scaled down by the
// remaining headroom fraction, so a nearly-full switch pauses earlier. The
// pause is latched per (inPort -> outPort) hop and idempotent: re-crossing
// while paused emits nothing (matching qbb-net-device's paused[] check in
// CheckShouldPause).
uint32_t
EthernetSwitch::EffectivePfcPauseThreshold(uint32_t outPort) const
{
    if (m_pfcMode == 2)
    {
        const uint32_t depth = GetVoqDepth();
        if (depth > 0)
        {
            // Headroom-aware scaling: threshold * (1 - occupancy). At zero
            // backlog this is exactly the static threshold (mode-2 == mode-1
            // baseline); as the VOQ fills, the effective pause point drops
            // toward zero — the same "pause earlier when the buffer is
            // scarce" direction as HPCC's (buffer_size - shared_used) >> shift.
            double occupancy = static_cast<double>(GetVoqSize(outPort))
                               / static_cast<double>(depth);
            double scaled = static_cast<double>(m_pfcPauseThreshold) * (1.0 - occupancy);
            return std::max<uint32_t>(1u, static_cast<uint32_t>(scaled));
        }
    }
    return m_pfcPauseThreshold;
}

void
EthernetSwitch::MaybeSendPfcPause(const FabricHeader& fh, uint32_t outPort,
                                  uint32_t inPort, size_t voqDepth)
{
    if (m_pfcMode == 0 || inPort >= GetNPorts() || outPort >= GetNPorts())
    {
        return;
    }
    // Only DATA-class So traffic pauses its source: control/retry/feedback
    // packets bypass (the same bypass class as ECN and the lossy drop —
    // pausing the feedback loop itself would deadlock it).
    if (fh.IsControlPacket() || fh.IsRetryPacket())
    {
        return;
    }
    if (IsPfcPaused(inPort, outPort))
    {
        return; // already paused toward this hop: idempotent
    }
    if (voqDepth < EffectivePfcPauseThreshold(outPort))
    {
        return;
    }

    // PAUSE toward the source behind this ingress. The FabricHeader carries
    // the (dest=source, source=dest) mirror of the original — the same
    // reversal EmitNackBack uses. The sequence-number field carries the
    // paused VOQ's depth (the PauseHeader qlen analog; zero quanta here
    // means "paused until an explicit RESUME", not the 802.1Qbb timer
    // expiry).
    FabricHeader pause;
    pause.SetPacketType(FabricPacketType::PFC_PAUSE);
    pause.SetFabricType(fh.GetFabricType());
    pause.SetSourceRank(fh.GetDestRank());
    pause.SetDestRank(fh.GetSourceRank());
    pause.SetSequenceNumber(static_cast<uint32_t>(voqDepth));
    pause.SetFlowId(fh.GetFlowId());
    pause.SetVirtualChannel(fh.GetVirtualChannel());
    pause.SetSourceMac(fh.GetDestMac());
    pause.SetDestMac(fh.GetSourceMac());

    Ptr<Packet> pausePkt = Create<Packet>(0);
    pausePkt->AddHeader(pause);
    GetPort(inPort)->Send(pausePkt, fh.GetSourceMac(), 0x0800);

    m_pfcPaused[{inPort, outPort}] = true;
    // Retain the mirrored header so the eventual RESUME (CheckPfcResume)
    // reverses exactly this pause: same ranks/MACs/fabric type.
    m_lastPfcPauseHeader[{inPort, outPort}] = pause;
    m_pfcPausesSent++;

    // Resume hysteresis watcher: poll the drained VOQ; lift the pause once
    // it falls to threshold - offset (HPCC's resume_offset guard against
    // thrash). Runs while paused; the ledger entry is retired at resume
    // (DoDispose cancels whatever is still pending). Prune the fired/expired
    // entries at each append — without pruning the ledger grew monotonically
    // (initial watcher + one EventId per 500-ns poll; 10,000 pause/resume
    // cycles retained 10,000 dead EventIds, and continuous congestion added
    // ~2M entries/s per paused hop — codex round 27, finding 9).
    EventId watcher = Simulator::Schedule(NanoSeconds(500),
                                          &EthernetSwitch::CheckPfcResume,
                                          this, inPort, outPort);
    PrunePfcResumeEvents();
    m_pfcResumeEvents.push_back(watcher);
}

void
EthernetSwitch::CheckPfcResume(uint32_t inPort, uint32_t outPort)
{
    if (!IsPfcPaused(inPort, outPort) || inPort >= GetNPorts())
    {
        return;
    }
    uint32_t threshold = EffectivePfcPauseThreshold(outPort);
    uint32_t resumeLevel = (threshold > m_pfcResumeOffset)
                               ? threshold - m_pfcResumeOffset
                               : 0;
    if (GetVoqSize(outPort) > resumeLevel)
    {
        // Still congested: keep watching (pruned at each append — see
        // MaybeSendPfcPause).
        EventId watcher = Simulator::Schedule(NanoSeconds(500),
                                              &EthernetSwitch::CheckPfcResume,
                                              this, inPort, outPort);
        PrunePfcResumeEvents();
        m_pfcResumeEvents.push_back(watcher);
        return;
    }
    // Drained past the hysteresis level: RESUME the paused source.
    m_pfcPaused[{inPort, outPort}] = false;

    // The RESUME re-targets whichever source the PAUSE silenced. The PAUSE
    // mirrored the ORIGINAL data header's ranks/MACs; replay that reversal
    // via m_lastPfcPauseHeader so the RESUME reaches the same endpoint.
    auto it = m_lastPfcPauseHeader.find({inPort, outPort});
    if (it != m_lastPfcPauseHeader.end())
    {
        FabricHeader resume;
        resume.SetPacketType(FabricPacketType::PFC_RESUME);
        resume.SetFabricType(it->second.GetFabricType());
        resume.SetSourceRank(it->second.GetSourceRank());
        resume.SetDestRank(it->second.GetDestRank());
        resume.SetSequenceNumber(0);
        resume.SetFlowId(it->second.GetFlowId());
        resume.SetVirtualChannel(it->second.GetVirtualChannel());
        resume.SetSourceMac(it->second.GetSourceMac());
        resume.SetDestMac(it->second.GetDestMac());

        Ptr<Packet> resumePkt = Create<Packet>(0);
        resumePkt->AddHeader(resume);
        GetPort(inPort)->Send(resumePkt, it->second.GetDestMac(), 0x0800);
        m_lastPfcPauseHeader.erase(it);
    }
    m_pfcResumesSent++;
}

bool
EthernetSwitch::OnPacketEnqueue(Ptr<Packet> packet, uint32_t outPort,
                                uint32_t inPort, size_t voqDepth)
{
    if (!packet)
    {
        return true;
    }
    FabricHeader fh;
    packet->PeekHeader(fh);

    // PFC (opt-in, default off): past the pause threshold, PAUSE the ingress
    // feeding this egress instead of dropping — the no-drop backstop RoCE
    // layers UNDER ECN (pause must fire after mark: EcnThreshold <
    // PfcPauseThreshold < VoqDropThreshold is the intended ordering).
    MaybeSendPfcPause(fh, outPort, inPort, voqDepth);

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
