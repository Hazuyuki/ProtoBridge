/*
 * SPDX-License-Identifier: GPL-2.0-only
 * ethernet-switch.h
 *
 * Ethernet (RoCE v2) switch for the scale-out (inter-node) fabric. A thin
 * NvSwitch derivative: reuses the L2 MAC-learning + VOQ + crossbar-arbiter +
 * cut-through datapath but carries no NVLink in-network compute (SHARP
 * all-reduce / NVLS all-gather are scale-up concepts with no Ethernet
 * equivalent; they default off on NvSwitch). RoCEv2 ECN congestion marking is
 * layered on here: on enqueue, when a VOQ crosses the ECN threshold, the
 * switch marks the inner SoHeader ECN bit of RoCE data packets (control
 * packets bypass, matching the BER bypass). PFC (IEEE 802.1Qbb-style priority
 * flow control) is an opt-in third arm: past a (higher) VOQ threshold the
 * switch PAUSEs the sources feeding the congested egress instead of dropping
 * (the no-drop backstop RoCE layers under ECN; HPCC's qbb-net-device is the
 * reference). DCQCN rate backoff is implemented in DcqcnRateMachine and
 * wired into FabricEndpoint (only the multi-flow incast e2e proof, R-SoH-3,
 * is deferred).
 */
#ifndef ETHERNET_SWITCH_H
#define ETHERNET_SWITCH_H

#include "fabric-header.h"
#include "nvswitch.h"

#include "ns3/event-id.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <utility>
#include <vector>

namespace ns3
{

/**
 * @ingroup gpu-cluster
 * @brief Ethernet (RoCEv2) switch for the scale-out fabric.
 *
 * Distinct TypeId ("ns3::EthernetSwitch") so the topology helpers can select
 * the So switch via NvSwitchHelper::SetSwitchType and the simulator can
 * distinguish an Su NVSwitch from an So Ethernet switch.
 */
class EthernetSwitch : public NvSwitch
{
  public:
    static TypeId GetTypeId();

    EthernetSwitch();
    ~EthernetSwitch() override = default;

    std::string GetVendorName() const override;

    /**
     * Mark the SoHeader ECN bit on a RoCE data packet when voqDepth >= the ECN
     * threshold. Control packets and non-RoCE packets are left untouched (they
     * carry no inner SoHeader). Returns true iff the packet was marked.
     *
     * Exposed publicly so the ECN-marking logic is unit-testable without
     * driving the full event-driven datapath.
     */
    bool MarkEcnIfCongested(Ptr<Packet> packet, size_t voqDepth) const;

    // NvSwitch congestion hook. For RoCE data packets this marks the SoHeader
    // ECN bit (lossless) when voqDepth crosses the ECN threshold; for plain
    // Ethernet data packets it tail-drops past the VoqDropThreshold and emits a
    // NACK back to the source (lossy, EmitNackBack). Control/retry packets
    // bypass (matching the BER bypass). Returns false to drop (caller counts a
    // VOQ drop), true to admit.
    //
    // Exposed publicly (overriding the protected NvSwitch base) so the
    // lossy-drop + NACK decision is unit-testable without driving the full
    // event-driven datapath -- mirroring MarkEcnIfCongested above.
    bool OnPacketEnqueue(Ptr<Packet> packet, uint32_t outPort,
                         uint32_t inPort, size_t voqDepth) override;

    /// Data packets whose SoHeader ECN bit this switch marked (observability).
    uint64_t GetEcnMarkedPackets() const { return m_ecnMarkedPackets; }

    /// PFC PAUSE frames this switch emitted (observability).
    uint64_t GetPfcPausesSent() const { return m_pfcPausesSent; }

    /// PFC RESUME frames this switch emitted (observability).
    uint64_t GetPfcResumesSent() const { return m_pfcResumesSent; }

    /**
     * Whether this switch currently holds a PFC pause toward (inPort ->
     * outPort). Exposed for tests and observability; latched per unidirectional
     * hop so one congested egress never silences unrelated sources.
     */
    bool IsPfcPaused(uint32_t inPort, uint32_t outPort) const;

  private:
    /// PFC trigger: past the (static or dynamic) threshold, PAUSE the ingress
    /// feeding the congested egress and arm the resume-hysteresis watcher.
    void MaybeSendPfcPause(const FabricHeader& fh, uint32_t outPort,
                           uint32_t inPort, size_t voqDepth);

    /// Resume-hysteresis poll (self-rescheduling while congested; emits the
    /// RESUME once the VOQ drains to threshold - offset).
    void CheckPfcResume(uint32_t inPort, uint32_t outPort);

    /// Mode-2 (dynamic) pause threshold; mode 1 returns the static value.
    uint32_t EffectivePfcPauseThreshold(uint32_t outPort) const;

  protected:
    void DoDispose() override;

  private:
    uint32_t m_ecnThreshold;     ///< VOQ depth (packets) at which ECN is marked
    uint32_t m_voqDropThreshold; ///< VOQ depth (packets) at which a plain-Ethernet
                                 ///< data packet is tail-dropped with a NACK
                                 ///< (lossy); must be < NvSwitch::VoqDepth
    uint64_t m_ecnMarkedPackets{0}; ///< count of ECN-marked data packets

    // ---- PFC (IEEE 802.1Qbb analog; opt-in, default off) ----
    uint32_t m_pfcMode;          ///< 0=off, 1=static threshold, 2=dynamic
                                 ///< (HPCC SwitchMmu-style, threshold follows
                                 ///< the remaining VOQ headroom)
    uint32_t m_pfcPauseThreshold;  ///< VOQ depth (packets) at which a PAUSE is
                                   ///< sent toward the sources of the
                                   ///< congested egress; must be <
                                   ///< VoqDropThreshold when both are armed
    uint32_t m_pfcResumeOffset;    ///< Hysteresis (packets): the pause lifts
                                   ///< when the VOQ drains to
                                   ///< pauseThreshold - resumeOffset
    uint64_t m_pfcPausesSent{0};   ///< PAUSE frames emitted
    uint64_t m_pfcResumesSent{0};  ///< RESUME frames emitted
    /// Latched pause state per (inPort -> outPort) hop; a hop is paused once
    /// and resumed once (idempotent re-trigger suppression).
    std::map<std::pair<uint32_t, uint32_t>, bool> m_pfcPaused;
    /// The mirrored header each PAUSE carried, per hop — the RESUME replays
    /// its reversal so it reaches the exact endpoint the pause silenced.
    std::map<std::pair<uint32_t, uint32_t>, FabricHeader> m_lastPfcPauseHeader;
    /// Resume-hysteresis watcher EventIds, one per paused (inPort, outPort)
    /// hop. Tracked so DoDispose cancels whatever is still pending (the
    /// callbacks target raw `this` and dereference m_voqs/m_ports, which
    /// DoDispose clears — the same ledger discipline as
    /// NvSwitch::m_deferredResultEvents).
    std::vector<EventId> m_pfcResumeEvents;
};

} // namespace ns3

#endif // ETHERNET_SWITCH_H
