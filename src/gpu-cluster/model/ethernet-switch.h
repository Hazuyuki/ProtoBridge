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
#include <set>
#include <tuple>
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
     * Configure a per-VC PFC pause threshold (the 802.1Qbb per-priority
     * headroom analog): traffic on vcId pauses at this VOQ depth instead of
     * the global PfcPauseThreshold. VCs without an entry keep the global
     * threshold. Inert when PfcMode=0. Setting a VC to the global value
     * (or erasing it) restores the shared-threshold behavior.
     */
    void SetPfcPauseThresholdForVc(uint8_t vcId, uint32_t threshold)
    {
        if (threshold == m_pfcPauseThreshold)
        {
            m_pfcPerVcThresholds.erase(vcId);
        }
        else
        {
            m_pfcPerVcThresholds[vcId] = threshold;
        }
    }

    /**
     * Whether this switch currently holds a PFC pause toward (inPort ->
     * outPort) on the given priority class (VC). Exposed for tests and
     * observability; latched per unidirectional hop per VC so one congested
     * egress never silences unrelated sources or unrelated priorities.
     */
    bool IsPfcPaused(uint32_t inPort, uint32_t outPort, uint8_t vcId) const;

    /// Compatibility overload: paused on ANY VC toward this hop.
    bool IsPfcPaused(uint32_t inPort, uint32_t outPort) const;

  private:
    /// PFC trigger: past the (static or dynamic) threshold, PAUSE the ingress
    /// feeding the congested egress and arm the resume-hysteresis watcher.
    void MaybeSendPfcPause(const FabricHeader& fh, uint32_t outPort,
                           uint32_t inPort, size_t voqDepth);

    /// Resume-hysteresis poll (self-rescheduling while congested; emits the
    /// RESUME once the VOQ drains to threshold - offset).
    void CheckPfcResume(uint32_t inPort, uint32_t outPort, uint8_t vcId);

    /// Bounded RESUME re-send after the first transmission (codex round 32,
    /// finding 1): the endpoint never acknowledges a RESUME, so a single
    /// lost frame left it paused FOREVER (the switch cleared its latch and
    /// dropped the mirrored header — no re-send path existed). The receiver
    /// treats a duplicate RESUME as a no-op when its dest is not paused, so
    /// re-sending is safe.
    void ResendPfcResume(uint32_t inPort, uint32_t outPort, uint8_t vcId);

    /// Mode 3 (quanta) only: re-check a PAUSE'd hop at pausetime/2 after
    /// the PAUSE was emitted — the endpoint's quanta timer will expire even
    /// if the VOQ is still congested, so the switch re-PAUSEs while the
    /// depth is still past the threshold (qbb-net-device's CheckQueueFull
    /// self-rescheduling) and emits a RESUME once it drains past the
    /// hysteresis level (the endpoint may still be latched from the quanta
    /// timer; a duplicate RESUME at an unpaused endpoint is a no-op).
    void RecheckPfcPause(uint32_t inPort, uint32_t outPort, uint8_t vcId);

    /// Drop fired watcher EventIds from m_pfcResumeEvents so the ledger
    /// tracks only LIVE polls (called at each append; codex round 27,
    /// finding 9).
    void PrunePfcResumeEvents();

    /// Mode-2 (dynamic) pause threshold; mode 1 returns the static value.
    uint32_t EffectivePfcPauseThreshold(uint32_t outPort) const;

    /// Per-(port, VC) PFC pause threshold; a VC with no configured entry
    /// uses the global PfcPauseThreshold. Inert in mode 0.
    uint32_t EffectivePfcPauseThreshold(uint32_t outPort, uint8_t vcId) const;

  protected:
    void DoDispose() override;

  private:
    uint32_t m_ecnThreshold;     ///< VOQ depth (packets) at which ECN is marked
    uint32_t m_voqDropThreshold; ///< VOQ depth (packets) at which a plain-Ethernet
                                 ///< data packet is tail-dropped with a NACK
                                 ///< (lossy); must be < NvSwitch::VoqDepth
    uint64_t m_ecnMarkedPackets{0}; ///< count of ECN-marked data packets

    // ---- PFC (IEEE 802.1Qbb analog; opt-in, default off) ----
    /// PFC latch key: (ingress port, egress port, priority class). The VC
    /// dimension is the 802.1Qbb priority analog — the FabricHeader's
    /// VirtualChannel field names the class, so per-VC keys need no wire
    /// format change.
    struct PfcHopKey
    {
        uint32_t inPort;  ///< ingress the paused traffic arrives on
        uint32_t outPort; ///< congested egress
        uint8_t vcId;     ///< priority class (FabricHeader VirtualChannel)

        bool operator<(const PfcHopKey& o) const
        {
            return std::tie(inPort, outPort, vcId)
                   < std::tie(o.inPort, o.outPort, o.vcId);
        }
    };

    uint32_t m_pfcMode;          ///< 0=off, 1=static threshold, 2=dynamic
                                 ///< (HPCC SwitchMmu-style, threshold follows
                                 ///< the remaining VOQ headroom), 3=quanta
                                 ///< (802.1Qbb pause-time semantics: the
                                 ///< PAUSE carries a finite pause time, the
                                 ///< endpoint auto-resumes on expiry, and
                                 ///< the switch re-checks every
                                 ///< pausetime/2 — qbb-net-device's
                                 ///< CheckQueueFull/PauseFinish contract)
    /// Mode 3 only: the pause time carried in the PAUSE frame's
    /// CreditCount field, in microseconds (the 802.1Qbb quanta analog;
    /// qbb-net-device's PauseTime). 0 = pause until an explicit RESUME
    /// (modes 1/2 semantics).
    uint32_t m_pfcPauseTimeUs{0};
    /// Mode 3 only: latched re-check state per hop+class — a PAUSE was
    /// emitted and the quanta re-check timer is armed (the endpoint's
    /// auto-resume may expire while the VOQ is still congested, so the
    /// switch must re-PAUSE, like qbb's CheckQueueFull self-rescheduling
    /// at pausetime/2).
    std::set<PfcHopKey> m_pfcQuantaArmed;
    uint32_t m_pfcPauseThreshold;  ///< VOQ depth (packets) at which a PAUSE is
                                   ///< sent toward the sources of the
                                   ///< congested egress; must be <
                                   ///< VoqDropThreshold when both are armed
    uint32_t m_pfcResumeOffset;    ///< Hysteresis (packets): the pause lifts
                                   ///< when the VOQ drains to
                                   ///< pauseThreshold - resumeOffset
    /// Per-VC pause thresholds (802.1Qbb per-priority headroom analog): a
    /// VC present here uses its entry instead of the global
    /// PfcPauseThreshold. Inert in mode 0; empty by default (every VC at
    /// the global threshold — byte-identical to the pre-VC behavior).
    std::map<uint8_t, uint32_t> m_pfcPerVcThresholds;
    uint64_t m_pfcPausesSent{0};   ///< PAUSE frames emitted
    uint64_t m_pfcResumesSent{0};  ///< RESUME frames emitted
    /// Latched pause state per (inPort -> outPort, VC) hop; a hop+class is
    /// paused once and resumed once (idempotent re-trigger suppression).
    std::map<PfcHopKey, bool> m_pfcPaused;
    /// The mirrored header each PAUSE carried, per hop+class — the RESUME
    /// replays its reversal so it reaches the exact endpoint the pause
    /// silenced. Kept for a bounded number of re-sends AFTER the first
    /// RESUME too (codex round 32, finding 1): a lost RESUME must be
    /// re-playable.
    std::map<PfcHopKey, FabricHeader> m_lastPfcPauseHeader;
    /// Re-sends remaining per hop+class (counts DOWN from the cap; the entry
    /// is retired when it hits zero). Retransmitting forever would grow the
    /// ledger on a dead link; the cap bounds the waste.
    std::map<PfcHopKey, uint32_t> m_pfcResumeResends;
    /// Resume-hysteresis watcher EventIds, one per paused (inPort, outPort)
    /// hop. Tracked so DoDispose cancels whatever is still pending (the
    /// callbacks target raw `this` and dereference m_voqs/m_ports, which
    /// DoDispose clears — the same ledger discipline as
    /// NvSwitch::m_deferredResultEvents).
    std::vector<EventId> m_pfcResumeEvents;
};

} // namespace ns3

#endif // ETHERNET_SWITCH_H
