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

// Bounded PFC RESUME re-sends after the first transmission (codex round 32,
// finding 1). The endpoint does not acknowledge RESUMEs, so the switch cannot
// tell delivered from lost; this caps the re-send chain so a dead link
// cannot grow the re-send ledger forever. 8 x 500 ns covers a transient loss
// window several µs long.
static constexpr uint32_t PFC_RESUME_RESEND_LIMIT = 8;

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
                          MakeUintegerChecker<uint32_t>(0, 3))
            .AddAttribute("PfcPauseThreshold",
                          "PFC: VOQ depth (packets) at which the switch "
                          "PAUSEs the ingress feeding the congested "
                          "egress (mode 1; the baseline for mode 2). "
                          "Inert when PfcMode=0.",
                          UintegerValue(64),
                          MakeUintegerAccessor(&EthernetSwitch::m_pfcPauseThreshold),
                          MakeUintegerChecker<uint32_t>(1))
            .AddAttribute("PfcPauseTime",
                          "PFC mode 3 (quanta) only: the pause time the "
                          "PAUSE frame carries (microseconds, in the "
                          "CreditCount field — the 802.1Qbb quanta / "
                          "qbb-net-device PauseTime analog). The endpoint "
                          "auto-resumes when the quanta expires; the "
                          "switch re-checks every PfcPauseTime/2 and "
                          "re-PAUSEs while the VOQ is still past the "
                          "threshold. 0 = pause until an explicit RESUME "
                          "(modes 1/2 semantics). Inert unless PfcMode=3. "
                          "Upper bound 65535: the quanta rides the "
                          "FabricHeader's 16-bit CreditCount field, so a "
                          "larger configured value would truncate on the "
                          "wire while the switch's re-check used the full "
                          "value (endpoint expiring at T mod 65536 while "
                          "the switch waits T/2).",
                          UintegerValue(0),
                          MakeUintegerAccessor(&EthernetSwitch::m_pfcPauseTimeUs),
                          MakeUintegerChecker<uint32_t>(0, 65535))
            .AddAttribute("PfcResumeOffset",
                          "PFC resume hysteresis (packets): the pause "
                          "lifts once the VOQ drains to "
                          "PfcPauseThreshold - PfcResumeOffset. Guards "
                          "against pause/resume thrash around the "
                          "threshold. Inert when PfcMode=0.",
                          UintegerValue(8),
                          MakeUintegerAccessor(&EthernetSwitch::m_pfcResumeOffset),
                          MakeUintegerChecker<uint32_t>(0))
            .AddAttribute("PfcHeadroomChunks",
                          "PFC headroom (packets) per paused hop: the "
                          "in-flight absorption budget for packets that "
                          "arrive AFTER the PAUSE (XOFF) was sent but "
                          "before the source actually stops — the link "
                          "RTT's worth of data plus any already-"
                          "serialized source burst. The 802.1Qbb / "
                          "ns3-rdma m_pg_hdrm_limit analog (qbb headroom). "
                          "While a hop is paused, its admissions draw "
                          "this budget instead of comparing the VOQ "
                          "threshold (already crossed); exhausting it "
                          "drops (the headroom-full drop). 0 = off "
                          "(legacy threshold comparisons). Size it >= "
                          "the path BDP: e.g. 40 Gbps x (2x1us + source "
                          "burst) is ~10 packets per hop.",
                          UintegerValue(0),
                          MakeUintegerAccessor(&EthernetSwitch::m_pfcHeadroomChunks),
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
EthernetSwitch::IsPfcPaused(uint32_t inPort, uint32_t outPort, uint8_t vcId) const
{
    auto it = m_pfcPaused.find({inPort, outPort, vcId});
    return it != m_pfcPaused.end() && it->second;
}

bool
EthernetSwitch::IsPfcPaused(uint32_t inPort, uint32_t outPort) const
{
    // Any VC latched toward this hop (the pre-VC compatibility view).
    for (auto it = m_pfcPaused.lower_bound({inPort, outPort, 0});
         it != m_pfcPaused.end() && it->first.inPort == inPort
         && it->first.outPort == outPort;
         ++it)
    {
        if (it->second)
        {
            return true;
        }
    }
    return false;
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
    m_pfcResumeResends.clear();
    m_pfcQuantaArmed.clear();
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

// Per-VC variant: the VC's configured threshold (or the global one) as the
// base, compared against the PER-VC VOQ depth (802.1Qbb pauses a priority
// class on its own occupancy, not the whole port's). The mode-2 headroom
// scaling keys on the whole-port occupancy exactly like the port-level
// variant (the shared-buffer pressure a class experiences is the port's).
uint32_t
EthernetSwitch::EffectivePfcPauseThreshold(uint32_t outPort, uint8_t vcId) const
{
    auto it = m_pfcPerVcThresholds.find(vcId);
    const uint32_t base =
        it != m_pfcPerVcThresholds.end() ? it->second : m_pfcPauseThreshold;
    if (m_pfcMode == 2)
    {
        const uint32_t depth = GetVoqDepth();
        if (depth > 0)
        {
            double occupancy = static_cast<double>(GetVoqSize(outPort))
                               / static_cast<double>(depth);
            double scaled = static_cast<double>(base) * (1.0 - occupancy);
            return std::max<uint32_t>(1u, static_cast<uint32_t>(scaled));
        }
    }
    return base;
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
    // Per-VC latch (the 802.1Qbb priority analog): the arriving packet's
    // FabricHeader VirtualChannel names the class. A pause on one VC never
    // silences another VC's traffic toward the same hop, and the idempotent
    // re-trigger suppression is per (hop, VC) too. The depth the threshold
    // compares against is the PER-VC occupancy (GetVoqSizeForVc) — a class
    // pauses on its own backlog, like qbb-net-device's per-priority
    // GetNBytes(j) check — EXCEPT when no per-VC thresholds are configured
    // at all, where the whole-port depth keeps the pre-VC semantics
    // byte-identical (single-class fabrics must not change behavior).
    const uint8_t vcId = fh.GetVirtualChannel();
    if (IsPfcPaused(inPort, outPort, vcId))
    {
        return; // already paused toward this hop on this class: idempotent
    }
    if (m_pfcPerVcThresholds.empty())
    {
        // Legacy single-class semantics: whole-port depth vs the global
        // threshold (byte-identical to the pre-VC behavior).
        if (voqDepth < EffectivePfcPauseThreshold(outPort))
        {
            return;
        }
    }
    else
    {
        if (GetVoqSizeForVc(outPort, vcId) < EffectivePfcPauseThreshold(outPort, vcId))
        {
            return;
        }
    }

    // PAUSE toward the source behind this ingress. The ranks carry the
    // (dest=source, source=dest) mirror of the original — the same reversal
    // EmitNackBack uses. The sequence-number field carries the paused VOQ's
    // depth (the PauseHeader qlen analog). Mode 3 (quanta) additionally
    // carries the pause time in the CreditCount field (the 802.1Qbb
    // quanta; 0 in modes 1/2 means "paused until an explicit RESUME").
    // The PAUSE's SourceMac is THIS SWITCH's own MAC (GetAddress), not the
    // mirrored end-to-end data MAC: the endpoint keys pause ownership by
    // (dest, SourceMac), and the end-to-end MACs are preserved across hops
    // — two switches pausing the same flow would emit the SAME mirrored
    // MAC, collapsing their holds into one entry again (the round-29 fix's
    // emission half, codex round 30, finding 1). DestMac keeps the mirror
    // (the endpoint's address) so the frame still routes to the paused
    // source; the RESUME replays the stored header verbatim, so it carries
    // the same switch MAC.
    FabricHeader pause;
    pause.SetPacketType(FabricPacketType::PFC_PAUSE);
    pause.SetFabricType(fh.GetFabricType());
    pause.SetSourceRank(fh.GetDestRank());
    pause.SetDestRank(fh.GetSourceRank());
    pause.SetSequenceNumber(static_cast<uint32_t>(voqDepth));
    pause.SetFlowId(fh.GetFlowId());
    pause.SetVirtualChannel(fh.GetVirtualChannel());
    pause.SetCreditCount(m_pfcMode == 3 ? static_cast<uint16_t>(m_pfcPauseTimeUs)
                                        : 0);
    pause.SetSourceMac(Mac48Address::ConvertFrom(GetAddress()));
    pause.SetDestMac(fh.GetSourceMac());

    Ptr<Packet> pausePkt = Create<Packet>(0);
    pausePkt->AddHeader(pause);
    GetPort(inPort)->Send(pausePkt, fh.GetSourceMac(), 0x0800);

    m_pfcPaused[{inPort, outPort, vcId}] = true;
    // Fresh pause cycle, fresh headroom budget: the XOFF just left this
    // switch, so the packets that will arrive before the source stops are
    // exactly the in-flight absorption this cycle is allowed to hold
    // (m_pfcHeadroomChunks; a re-latch after a mode-3 quanta expiry starts
    // a new cycle, not a continuation of the old one).
    m_pfcHeadroomUsed[{inPort, outPort, vcId}] = 0;
    // Retain the mirrored header so the eventual RESUME (CheckPfcResume)
    // reverses exactly this pause: same ranks/MACs/fabric type.
    m_lastPfcPauseHeader[{inPort, outPort, vcId}] = pause;
    // A fresh pause cycle retires any stale re-send budget from the
    // previous cycle's RESUME (the new watcher owns the hop's resume
    // state; the old chain's next fire would see IsPfcPaused and stop,
    // but retiring here keeps the maps from holding dead entries).
    m_pfcResumeResends.erase({inPort, outPort, vcId});
    m_pfcPausesSent++;

    if (m_pfcMode == 3)
    {
        // Quanta mode (qbb-net-device's CheckQueueFull contract): the
        // endpoint's quanta timer owns the un-pause, so the switch does
        // NOT poll for drain here — it re-checks at pausetime/2 instead,
        // re-PAUSEing if the VOQ is still past the threshold (the quanta
        // will have expired by then) and emitting a RESUME once drained.
        // The re-check runs on the same pruned ledger as the watchers.
        m_pfcQuantaArmed.insert({inPort, outPort, vcId});
        const uint64_t recheckNs = m_pfcPauseTimeUs > 0
                                       ? (m_pfcPauseTimeUs * 1000ULL) / 2
                                       : 500;
        EventId recheck = Simulator::Schedule(
            NanoSeconds(recheckNs), &EthernetSwitch::RecheckPfcPause,
            this, inPort, outPort, vcId);
        PrunePfcResumeEvents();
        m_pfcResumeEvents.push_back(recheck);
        return;
    }

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
                                          this, inPort, outPort, vcId);
    PrunePfcResumeEvents();
    m_pfcResumeEvents.push_back(watcher);
}

void
EthernetSwitch::RecheckPfcPause(uint32_t inPort, uint32_t outPort, uint8_t vcId)
{
    // The re-check timer fired. Quanta mode's lifecycle: the endpoint's
    // quanta may have expired (endpoint un-paused) or may still hold. The
    // switch's OWN latch reflects what it last told the endpoint; if the
    // VOQ is still congested we re-PAUSE (idempotent at the endpoint: a
    // PAUSE for an already-paused (dest, vc, switch) just re-latches and
    // re-arms the endpoint's timer); if it drained, we un-latch and send a
    // RESUME (a no-op if the endpoint's quanta already expired).
    m_pfcQuantaArmed.erase({inPort, outPort, vcId});
    if (inPort >= GetNPorts())
    {
        return;
    }
    const size_t depth = m_pfcPerVcThresholds.empty()
                             ? GetVoqSize(outPort)
                             : GetVoqSizeForVc(outPort, vcId);
    const uint32_t threshold = m_pfcPerVcThresholds.empty()
                                   ? EffectivePfcPauseThreshold(outPort)
                                   : EffectivePfcPauseThreshold(outPort, vcId);
    uint32_t resumeLevel = (threshold > m_pfcResumeOffset)
                               ? threshold - m_pfcResumeOffset
                               : 0;
    if (depth > resumeLevel)
    {
        // Still congested: re-PAUSE (the endpoint's quanta is expiring or
        // has expired — this refreshes the hold), then re-arm the re-check.
        auto it = m_lastPfcPauseHeader.find({inPort, outPort, vcId});
        if (it != m_lastPfcPauseHeader.end())
        {
            FabricHeader pause = it->second;
            Ptr<Packet> pausePkt = Create<Packet>(0);
            pausePkt->AddHeader(pause);
            GetPort(inPort)->Send(pausePkt, pause.GetDestMac(), 0x0800);
            m_pfcPausesSent++;
        }
        m_pfcQuantaArmed.insert({inPort, outPort, vcId});
        // Fresh quanta cycle, fresh headroom budget (mirrors the latch in
        // MaybeSendPfcPause — the re-PAUSE is a new XOFF in flight).
        m_pfcHeadroomUsed[{inPort, outPort, vcId}] = 0;
        const uint64_t recheckNs = m_pfcPauseTimeUs > 0
                                       ? (m_pfcPauseTimeUs * 1000ULL) / 2
                                       : 500;
        EventId recheck = Simulator::Schedule(
            NanoSeconds(recheckNs), &EthernetSwitch::RecheckPfcPause,
            this, inPort, outPort, vcId);
        PrunePfcResumeEvents();
        m_pfcResumeEvents.push_back(recheck);
        return;
    }
    // Drained: un-latch and RESUME (a duplicate at an endpoint whose quanta
    // already expired is a no-op). No re-check re-arm — the next congestion
    // crossing re-PAUSEs from MaybeSendPfcPause.
    m_pfcPaused[{inPort, outPort, vcId}] = false;
    // The pause cycle is over — retire any leftover headroom accounting so
    // the next cycle (a fresh latch in MaybeSendPfcPause) starts clean.
    m_pfcHeadroomUsed.erase({inPort, outPort, vcId});
    auto it = m_lastPfcPauseHeader.find({inPort, outPort, vcId});
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
        // Bounded re-send arm — the same contract as the mode-1 drain
        // watcher (codex round 32, finding 1; missing here per codex round
        // 34, finding 3): the endpoint never acknowledges a RESUME, so a
        // lost mode-3 RESUME with pfcPauseTime=0 left the endpoint paused
        // forever (the switch had cleared its latch with no expiry coming).
        // With positive quanta the expiry eventually lifts the hold, but
        // recovery waits the full quanta instead of 500 ns.
        m_pfcResumeResends[{inPort, outPort, vcId}] = PFC_RESUME_RESEND_LIMIT;
        EventId resend = Simulator::Schedule(
            NanoSeconds(500), &EthernetSwitch::ResendPfcResume,
            this, inPort, outPort, vcId);
        PrunePfcResumeEvents();
        m_pfcResumeEvents.push_back(resend);
        m_pfcResumesSent++;
    }
}

void
EthernetSwitch::CheckPfcResume(uint32_t inPort, uint32_t outPort, uint8_t vcId)
{
    if (!IsPfcPaused(inPort, outPort, vcId) || inPort >= GetNPorts())
    {
        return;
    }
    // Drained-past-hysteresis test on the same depth the pause latched on:
    // per-VC depth when per-VC thresholds are configured, whole-port depth
    // otherwise (the single-class legacy view — byte-identical resume
    // behavior for fabrics that never configured per-VC PFC).
    const size_t depth = m_pfcPerVcThresholds.empty()
                             ? GetVoqSize(outPort)
                             : GetVoqSizeForVc(outPort, vcId);
    const uint32_t threshold = m_pfcPerVcThresholds.empty()
                                   ? EffectivePfcPauseThreshold(outPort)
                                   : EffectivePfcPauseThreshold(outPort, vcId);
    uint32_t resumeLevel = (threshold > m_pfcResumeOffset)
                               ? threshold - m_pfcResumeOffset
                               : 0;
    if (depth > resumeLevel)
    {
        // Still congested: keep watching (pruned at each append — see
        // MaybeSendPfcPause).
        EventId watcher = Simulator::Schedule(NanoSeconds(500),
                                              &EthernetSwitch::CheckPfcResume,
                                              this, inPort, outPort, vcId);
        PrunePfcResumeEvents();
        m_pfcResumeEvents.push_back(watcher);
        return;
    }
    // Drained past the hysteresis level: RESUME the paused source.
    m_pfcPaused[{inPort, outPort, vcId}] = false;
    // Pause cycle over — retire the headroom accounting with the latch.
    m_pfcHeadroomUsed.erase({inPort, outPort, vcId});

    // The RESUME re-targets whichever source the PAUSE silenced. The PAUSE
    // mirrored the ORIGINAL data header's ranks/MACs; replay that reversal
    // via m_lastPfcPauseHeader so the RESUME reaches the same endpoint.
    auto it = m_lastPfcPauseHeader.find({inPort, outPort, vcId});
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
        // Bounded re-send arm (codex round 32, finding 1): the endpoint
        // never acknowledges a RESUME, so the switch cannot distinguish
        // delivered from lost. A single-shot RESUME that the link
        // degradation model dropped left the endpoint paused FOREVER —
        // the switch had cleared its latch and dropped the mirrored
        // header, with no re-send path (codex probe: switch_paused=0,
        // endpoint_paused=1, delivered=0, and the LLR store empty after
        // 1 ms). Keep the header for a bounded number of re-sends; a
        // duplicate RESUME at an unpaused endpoint is a no-op
        // (ProcessPfcResumePacket early-returns when the dest is not in
        // its latch), so re-sending is safe on a healthy link too.
        m_pfcResumeResends[{inPort, outPort, vcId}] = PFC_RESUME_RESEND_LIMIT;
        EventId resend = Simulator::Schedule(
            NanoSeconds(500), &EthernetSwitch::ResendPfcResume,
            this, inPort, outPort, vcId);
        PrunePfcResumeEvents();
        m_pfcResumeEvents.push_back(resend);
    }
    m_pfcResumesSent++;
}

void
EthernetSwitch::ResendPfcResume(uint32_t inPort, uint32_t outPort, uint8_t vcId)
{
    if (inPort >= GetNPorts())
    {
        return;
    }
    auto budget = m_pfcResumeResends.find({inPort, outPort, vcId});
    if (budget == m_pfcResumeResends.end())
    {
        // A new pause cycle (or Dispose) already retired this hop's budget.
        return;
    }
    if (budget->second == 0)
    {
        // Budget exhausted: retire.
        m_pfcResumeResends.erase(budget);
        m_lastPfcPauseHeader.erase({inPort, outPort, vcId});
        return;
    }
    budget->second--;
    auto it = m_lastPfcPauseHeader.find({inPort, outPort, vcId});
    if (it == m_lastPfcPauseHeader.end())
    {
        m_pfcResumeResends.erase(budget);
        return;
    }
    // A NEW pause cycle re-armed the watcher: it owns the hop's resume
    // state now — stop the stale re-send chain (the fresh watcher will
    // RESUME when the VOQ drains).
    if (IsPfcPaused(inPort, outPort, vcId))
    {
        m_pfcResumeResends.erase(budget);
        return;
    }
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
    m_pfcResumesSent++;
    if (budget->second > 0)
    {
        EventId resend = Simulator::Schedule(
            NanoSeconds(500), &EthernetSwitch::ResendPfcResume,
            this, inPort, outPort, vcId);
        PrunePfcResumeEvents();
        m_pfcResumeEvents.push_back(resend);
    }
    else
    {
        m_pfcResumeResends.erase(budget);
        m_lastPfcPauseHeader.erase({inPort, outPort, vcId});
    }
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

    // PFC headroom (XOFF in-flight absorption, ns3-rdma m_pg_hdrm_limit
    // analog): while this hop is PAUSEd, data-class admissions draw a finite
    // per-hop budget instead of admitting unbounded toward the base VoqDepth
    // hard cap. The XOFF needs a link RTT (+ any already-serialized source
    // burst) to actually stop the source; those packets are exactly what the
    // budget holds. Checked BEFORE the pause trigger so the threshold-
    // crossing packet itself (admitted to the shared buffer in ns3-rdma's
    // model) does not draw the budget — only post-XOFF arrivals do.
    // Exhaustion drops (m_pfcHeadroomDrops — the "ingress headroom full"
    // analog). Control/retry bypass: pausing feedback would deadlock the
    // loop, same exemption as every other congestion hook. 0 = off (legacy
    // unbounded admission while paused).
    if (m_pfcHeadroomChunks > 0 && !fh.IsControlPacket() && !fh.IsRetryPacket())
    {
        const PfcHopKey key{inPort, outPort, fh.GetVirtualChannel()};
        if (IsPfcPaused(inPort, outPort, fh.GetVirtualChannel()))
        {
            auto it = m_pfcHeadroomUsed.find(key);
            const uint32_t used = (it != m_pfcHeadroomUsed.end()) ? it->second : 0;
            if (used >= m_pfcHeadroomChunks)
            {
                m_pfcHeadroomDrops++;
                NS_LOG_WARN("PFC headroom exhausted for hop " << inPort << "->"
                              << outPort << " vc " << static_cast<int>(fh.GetVirtualChannel())
                              << ", dropping packet");
                // NACK like the lossy drop below: a silent drop in a
                // lossless fabric is an unrecoverable loss (h=10 with no
                // NACK deadlocks the transfer — verified). The LLR/SACK
                // retry path is the recovery, exactly as for tail-drops.
                EmitNackBack(fh, inPort, fh.GetSourceMac());
                return false; // drop: caller skips the push + counts m_voqDrops
            }
            if (it != m_pfcHeadroomUsed.end())
            {
                it->second = used + 1;
            }
            else
            {
                m_pfcHeadroomUsed[key] = 1;
            }
        }
    }

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
